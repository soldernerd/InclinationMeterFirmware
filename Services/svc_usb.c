#include "svc_usb.h"
#include "svc_api.h"
#include "svc_log.h"
#include "svc_txframe.h"
#include "hal_usb.h"
#include "config.h"
#include "system_state.h"
#include <string.h>

/* USB transport for svc_api (API v3), mirroring svc_ble.c / svc_uart.c.
 *
 * Framing on top of the fixed 64-byte HID reports (docs/api-v3-spec.md section 2.2): an API packet always starts at
 * the beginning of a report and occupies ceil((6 + LEN) / 64) consecutive reports, the last one zero-padded; the
 * receiver learns the packet length from the LEN field in the first report. Both directions use it.
 *
 * RX: the HAL USB ISR fires hal_usb_on_rx() -> rx_handler() here; reports are collected into one packet buffer and
 * the scheduler tick is flagged when it is complete (svc_api_receive is never called from interrupt context).
 *
 * TX: svc_api hands us an exact 6+LEN packet via send_via_usb(), which enqueues it in a per-transport TX frame ring
 * (CLAUDE.md 8.3). usb_tx_pump() then feeds the frame to hal_usb_send() one report at a time as fast as the host
 * picks them up, so a slow host cannot stall the dispatcher. */

static volatile bool     s_rx_pending    = false;
static          uint8_t  s_rx_buf[API2_PACKET_MAX_SIZE];
static volatile uint16_t s_rx_len        = 0;        /* complete packet length once s_rx_pending */
static          uint16_t s_rx_fill       = 0;        /* bytes collected so far (ISR-owned) */
static          uint16_t s_rx_total      = 0;        /* length of the packet being collected, 0 = none */
static          uint16_t s_tx_off        = 0;        /* bytes of the frame at the ring front already sent */
static          bool     s_was_connected = false;

static SvcTxFrame        s_tx;
static uint8_t           s_tx_buf[API_TX_RING_SIZE];
static uint8_t           s_stage[API2_PACKET_MAX_SIZE];
static bool              s_tx_overflowed;   /* edge flag: one WARN per full episode */

static void rx_handler(const uint8_t *data, uint16_t len)
{
    if (s_rx_pending) {
        /* Previous packet not yet processed — drop. svc_api hosts are
         * expected to wait for the reply before sending the next command. */
        return;
    }
    uint16_t copy = len > USB_HID_REPORT_SIZE ? USB_HID_REPORT_SIZE : len;
    if (s_rx_total == 0U) {
        /* first report of a packet: LEN is in bytes 2..3 */
        if (copy < API2_PACKET_HDR_BYTES) {
            return;
        }
        uint32_t total = (uint32_t)API2_PACKET_HDR_BYTES + (uint16_t)(data[2] | ((uint16_t)data[3] << 8))
                       + API2_PACKET_CRC_BYTES;
        if (total > API2_PACKET_MAX_SIZE) {
            return;   /* oversized: ignore (svc_api counts malformed frames it sees, this one never reaches it) */
        }
        s_rx_total = (uint16_t)total;
        s_rx_fill  = 0;
    }
    uint16_t room = (uint16_t)(s_rx_total - s_rx_fill);
    if (copy > room) copy = room;      /* the zero padding of the last report is not part of the packet */
    memcpy(&s_rx_buf[s_rx_fill], data, copy);
    s_rx_fill = (uint16_t)(s_rx_fill + copy);
    if (s_rx_fill >= s_rx_total) {
        s_rx_len     = s_rx_total;
        s_rx_total   = 0;
        s_rx_pending = true;
    }
}

static void usb_tx_pump(void)
{
    if (!hal_usb_is_connected()) {
        return;
    }
    for (;;) {
        uint16_t n = svc_txframe_peek(&s_tx, s_stage, sizeof s_stage);
        if (n == 0) {
            s_tx_overflowed = false;   /* drained -- re-arm the WARN */
            s_tx_off = 0;
            break;
        }
        /* One report per iteration; a frame longer than 64 bytes continues in the next report. */
        while (s_tx_off < n) {
            uint16_t chunk = (uint16_t)(n - s_tx_off);
            if (chunk > USB_HID_REPORT_SIZE) chunk = USB_HID_REPORT_SIZE;
            if (!hal_usb_send(&s_stage[s_tx_off], chunk)) {
                return;                /* USBD_BUSY -- retry from this offset next tick */
            }
            s_tx_off = (uint16_t)(s_tx_off + chunk);
        }
        s_tx_off = 0;
        svc_txframe_drop_front(&s_tx);
    }
}

/* CLAUDE.md 7.6 — No Silent Failures: a frame the TX ring can't take is
 * genuinely lost (no retry queue). Escalate to
 * g_system_state.usb_tx_dropped_count and emit one WARN per full episode,
 * rather than discarding it silently. */
static void send_via_usb(const uint8_t *data, uint16_t len, bool urgent)
{
    if (!svc_txframe_push(&s_tx, data, len, urgent)) {
        if (g_system_state.usb_tx_dropped_count < UINT16_MAX) {
            g_system_state.usb_tx_dropped_count++;
        }
        if (!s_tx_overflowed) {
            s_tx_overflowed = true;
            svc_log(API2_LOG_WARN, "usb: tx ring full, frame dropped");
        }
        return;
    }
    usb_tx_pump();
}

void svc_usb_init(void)
{
    s_rx_pending    = false;
    s_rx_len        = 0;
    s_rx_fill       = 0;
    s_rx_total      = 0;
    s_tx_off        = 0;
    s_was_connected = false;
    s_tx_overflowed = false;

    svc_txframe_init(&s_tx, s_tx_buf, sizeof s_tx_buf);
    svc_api_register_transport(API_TRANSPORT_USB, send_via_usb);
    hal_usb_register_rx_callback(rx_handler);
    hal_usb_init();
}

void svc_usb_update(void)
{
    hal_usb_update();

    bool now_connected = hal_usb_is_connected();
    if (now_connected && !s_was_connected) {
        svc_txframe_reset(&s_tx);
        s_tx_overflowed = false;
        s_tx_off   = 0;
        s_rx_total = 0;
        svc_api_connected(API_TRANSPORT_USB);
        svc_log(API2_LOG_INFO, "usb: host connected");
    } else if (!now_connected && s_was_connected) {
        svc_api_disconnected(API_TRANSPORT_USB);
        svc_txframe_reset(&s_tx);   /* queued frames are for a gone host */
        s_tx_overflowed = false;
        s_tx_off   = 0;
        s_rx_total = 0;
        svc_log(API2_LOG_INFO, "usb: host disconnected");
    }
    s_was_connected = now_connected;

    /* NOTE: g_system_state.usb_connected is deliberately NOT written here.
     * It means "USB power present (VBUS)" and is owned solely by
     * svc_battery.c. This module's enumeration state (now_connected) is a
     * different thing — a host attached and enumerated — and stays local;
     * anything needing it calls hal_usb_is_connected(). Previously both
     * wrote the same field on different tick periods, so it flapped. */

    if (s_rx_pending) {
        uint16_t len = s_rx_len;
        svc_api_receive(API_TRANSPORT_USB, s_rx_buf, len);
        s_rx_pending = false;
    }

    usb_tx_pump();
}
