#include "svc_usb.h"
#include "svc_api.h"
#include "svc_log.h"
#include "svc_txframe.h"
#include "hal_usb.h"
#include "config.h"
#include "system_state.h"
#include <string.h>

/* USB transport for svc_api (API v2), mirroring svc_ble.c / svc_uart.c.
 *
 * RX: the HAL USB ISR fires hal_usb_on_rx() -> rx_handler() here; we copy
 * the report bytes and flag the scheduler tick to drain (svc_api_receive
 * is never called from interrupt context). One HID OUT report is one
 * complete request packet at current payload sizes.
 *
 * TX: svc_api hands us an exact 6+LEN packet via send_via_usb(), which
 * enqueues it in a per-transport TX frame ring (CLAUDE.md §8.3).
 * usb_tx_pump() then feeds frames to hal_usb_send() (which pads each to
 * the fixed 64-byte HID IN report) as fast as the host picks them up,
 * so a slow host cannot stall the dispatcher. */

static volatile bool     s_rx_pending    = false;
static          uint8_t  s_rx_buf[USB_HID_REPORT_SIZE];
static volatile uint16_t s_rx_len        = 0;
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
    memcpy(s_rx_buf, data, copy);
    s_rx_len     = copy;
    s_rx_pending = true;
}

static void usb_tx_pump(void)
{
    if (!hal_usb_is_connected()) {
        return;
    }
    for (;;) {
        uint16_t n = svc_txframe_peek(&s_tx, s_stage, sizeof s_stage);
        if (n == 0) {
            s_tx_overflowed = false;   /* drained — re-arm the WARN */
            break;
        }
        if (!hal_usb_send(s_stage, n)) {
            break;                     /* USBD_BUSY — retry next tick */
        }
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
    s_was_connected = false;
    s_tx_overflowed = false;

    svc_txframe_init(&s_tx, s_tx_buf, sizeof s_tx_buf);
    svc_api_register_transport(API_TRANSPORT_USB, send_via_usb);
    hal_usb_register_rx_callback(rx_handler);
    hal_usb_init();
}

bool svc_usb_is_connected(void)
{
    return hal_usb_is_connected();
}

void svc_usb_update(void)
{
    hal_usb_update();

    bool now_connected = hal_usb_is_connected();
    if (now_connected && !s_was_connected) {
        svc_txframe_reset(&s_tx);
        s_tx_overflowed = false;
        svc_api_connected(API_TRANSPORT_USB);
        svc_log(API2_LOG_INFO, "usb: host connected");
    } else if (!now_connected && s_was_connected) {
        svc_api_disconnected(API_TRANSPORT_USB);
        svc_txframe_reset(&s_tx);   /* queued frames are for a gone host */
        s_tx_overflowed = false;
        svc_log(API2_LOG_INFO, "usb: host disconnected");
    }
    s_was_connected = now_connected;

    g_system_state.usb_connected = now_connected;

    if (s_rx_pending) {
        uint16_t len = s_rx_len;
        svc_api_receive(API_TRANSPORT_USB, s_rx_buf, len);
        s_rx_pending = false;
    }

    usb_tx_pump();
}
