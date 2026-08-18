#include "svc_uart.h"
#include "svc_api.h"
#include "hal_uart.h"
#include "config.h"
#include "system_state.h"

/* Wired debug/VCP UART transport adapter (USART3, PD8/PD9, WP5.1). Owns:
 *   - registration with svc_api as API_TRANSPORT_UART
 *   - reassembly of incoming bytes into packets, via svc_api.c's shared
 *     ApiByteReassembler (also used by Services/svc_ble.c)
 *
 * Unlike USB (VBUS-based enumeration) or BLE (RN4871 connect/disconnect
 * events), a wired UART has no peer-present signal to poll: either
 * something is listening on the other end of the wire or it isn't. So
 * this transport is simply marked connected once, at init, and never
 * disconnected — svc_api_receive()/send_via_uart() work the same whether
 * or not a host is actually attached; an unattached line just means
 * nothing is ever sent or received on it. */

static ApiByteReassembler s_reassembler;

/* CLAUDE.md 7.6 — No Silent Failures: hal_uart_write() can fail (TX DMA
 * already busy) and there's no retry queue, so a failed send here is
 * genuinely lost. Escalate to g_system_state.uart_tx_dropped_count rather
 * than silently discarding the result — mirrors Services/svc_ble.c's and
 * Services/svc_usb.c's equivalents. */
static void send_via_uart(const uint8_t *data, uint16_t len)
{
    if (hal_uart_write(HAL_UART_DEBUG, data, len) != DRV_OK) {
        if (g_system_state.uart_tx_dropped_count < UINT16_MAX) {
            g_system_state.uart_tx_dropped_count++;
        }
    }
}

void svc_uart_init(void)
{
    s_reassembler.pos = 0;

    svc_api_register_transport(API_TRANSPORT_UART, send_via_uart);
    svc_api_connected(API_TRANSPORT_UART);
}

void svc_uart_update(void)
{
    uint8_t b;
    while (hal_uart_read_byte(HAL_UART_DEBUG, &b)) {
        svc_api_reassembler_feed_byte(API_TRANSPORT_UART, &s_reassembler, b);
    }
    svc_api_reassembler_check_timeout(&s_reassembler, API_RX_PACKET_TIMEOUT_MS);
}
