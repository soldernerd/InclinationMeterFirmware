#include "svc_usb.h"
#include "svc_api.h"
#include "hal_usb.h"
#include "config.h"
#include "system_state.h"
#include <string.h>

/* Single-buffer RX queue. The HAL USB ISR fires hal_usb_on_rx() which
 * forwards here; we copy the bytes and set a flag for the scheduler tick
 * to drain. We don't call svc_api_receive() from interrupt context. */
static volatile bool     s_rx_pending    = false;
static          uint8_t  s_rx_buf[USB_HID_REPORT_SIZE];
static volatile uint16_t s_rx_len        = 0;
static          bool     s_was_connected = false;

static void rx_handler(const uint8_t *data, uint16_t len)
{
    if (s_rx_pending) {
        /* Previous packet not yet processed — drop. svc_api hosts are
         * expected to ACK before sending the next command. */
        return;
    }
    uint16_t copy = len > USB_HID_REPORT_SIZE ? USB_HID_REPORT_SIZE : len;
    memcpy(s_rx_buf, data, copy);
    s_rx_len     = copy;
    s_rx_pending = true;
}

/* CLAUDE.md 7.6 — No Silent Failures: hal_usb_send() can fail (not
 * connected, or USBD_BUSY — a previous IN report still in flight) and
 * there's no retry queue, so a failed send here is genuinely lost.
 * Escalate to g_system_state.usb_tx_dropped_count rather than silently
 * discarding the result — see that field's own comment. */
static void send_via_usb(const uint8_t *data, uint16_t len)
{
    if (!hal_usb_send(data, len)) {
        if (g_system_state.usb_tx_dropped_count < UINT16_MAX) {
            g_system_state.usb_tx_dropped_count++;
        }
    }
}

void svc_usb_init(void)
{
    s_rx_pending    = false;
    s_rx_len        = 0;
    s_was_connected = false;

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
        svc_api_connected(API_TRANSPORT_USB);
    } else if (!now_connected && s_was_connected) {
        svc_api_disconnected(API_TRANSPORT_USB);
    }
    s_was_connected = now_connected;

    g_system_state.usb_connected = now_connected;

    if (s_rx_pending) {
        /* Snapshot len then clear pending so a re-entrant ISR can fill
         * the buffer for the next tick. */
        uint16_t len = s_rx_len;
        svc_api_receive(API_TRANSPORT_USB, s_rx_buf, len);
        s_rx_pending = false;
    }
}
