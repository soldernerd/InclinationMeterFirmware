#include "svc_uart.h"
#include "svc_api.h"
#include "svc_log.h"
#include "svc_txframe.h"
#include "hal_uart.h"
#include "config.h"
#include "system_state.h"

/* See svc_uart.h. Mirrors svc_ble.c: inbound bytes go through svc_api's
 * shared ApiByteReassembler; outbound frames queue in a per-transport TX
 * frame ring (CLAUDE.md §8.3) drained one at a time by uart_tx_pump() as
 * each DMA transfer completes, so the dispatcher never blocks on the
 * wire. hal_uart_init(HAL_UART_DEBUG) is done in main()'s boot sequence
 * next to the other HAL inits. */

static SvcTxFrame         s_tx;
static uint8_t            s_tx_buf[API_TX_RING_SIZE];
static uint8_t            s_stage[API2_PACKET_MAX_SIZE];
static bool               s_tx_overflowed;   /* edge flag: one WARN per full episode */
static ApiByteReassembler s_reasm;

static void uart_tx_pump(void)
{
    if (!hal_uart_tx_idle(HAL_UART_DEBUG)) {
        return;
    }
    if (svc_txframe_is_empty(&s_tx)) {
        s_tx_overflowed = false;   /* drained — re-arm the WARN */
        return;
    }
    uint16_t n = svc_txframe_pop(&s_tx, s_stage, sizeof s_stage);
    if (n) {
        (void)hal_uart_write(HAL_UART_DEBUG, s_stage, n); /* tx_idle checked -> accepted */
    }
}

/* CLAUDE.md 7.6 — No Silent Failures: a frame the TX ring can't take is
 * lost (no retry queue). Escalate to g_system_state.uart_tx_dropped_count
 * and emit one WARN per full episode. */
static void send_via_uart(const uint8_t *data, uint16_t len, bool urgent)
{
    if (!svc_txframe_push(&s_tx, data, len, urgent)) {
        if (g_system_state.uart_tx_dropped_count < UINT16_MAX) {
            g_system_state.uart_tx_dropped_count++;
        }
        if (!s_tx_overflowed) {
            s_tx_overflowed = true;
            svc_log(API2_LOG_WARN, "uart: tx ring full, frame dropped");
        }
        return;
    }
    uart_tx_pump();
}

void svc_uart_init(void)
{
    s_tx_overflowed = false;
    s_reasm.pos     = 0;
    svc_txframe_init(&s_tx, s_tx_buf, sizeof s_tx_buf);
    svc_api_register_transport(API_TRANSPORT_UART, send_via_uart);
    svc_api_connected(API_TRANSPORT_UART);   /* wired link: connected for good */
}

void svc_uart_update(void)
{
    uint8_t  buf[64];
    uint16_t n;
    while ((n = hal_uart_read(HAL_UART_DEBUG, buf, sizeof buf)) > 0U) {
        for (uint16_t i = 0; i < n; i++) {
            svc_api_reassembler_feed_byte(API_TRANSPORT_UART, &s_reasm, buf[i]);
        }
    }
    svc_api_reassembler_check_timeout(&s_reasm, API_RX_PACKET_TIMEOUT_MS);

    uart_tx_pump();
}
