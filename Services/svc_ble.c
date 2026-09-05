#include "svc_ble.h"
#include "svc_api.h"
#include "svc_log.h"
#include "svc_txframe.h"
#include "drv_rn4871.h"
#include "hal_uart.h"
#include "config.h"
#include "system_state.h"

/* BLE transport for svc_api (API v2), mirroring svc_usb.c / svc_uart.c.
 * The RN4871 Transparent UART is a raw byte stream, so inbound bytes are
 * fed into svc_api's shared ApiByteReassembler to rebuild
 * [OPCODE][LEN][PAYLOAD][CRC16] packets before dispatch.
 *
 * Outbound, svc_api builds an exact 6+LEN packet (v2 has no padding) and
 * calls send_via_ble(), which enqueues it in a per-transport TX frame
 * ring (CLAUDE.md §8.3). ble_tx_pump() then moves one frame at a time
 * onto the wire via the RN4871 as each DMA transfer drains — the
 * dispatcher never blocks on a slow or stalled central. The pump runs
 * once per svc_ble_task() and again right after each enqueue so a lone
 * command response still goes out the same tick.
 *
 * The RN4871 config/reset state machine is pumped from drv_rn4871_task()
 * inside svc_ble_task(); nothing blocks. */

static SvcTxFrame        s_tx;
static uint8_t           s_tx_buf[API_TX_RING_SIZE];
static uint8_t           s_stage[API2_PACKET_MAX_SIZE];
static bool              s_tx_overflowed;   /* edge flag: one WARN per full episode */
static ApiByteReassembler s_reasm;
static bool              s_was_connected;

static void ble_tx_pump(void)
{
    if (!drv_rn4871_is_connected())      return;
    if (!hal_uart_tx_idle(HAL_UART_BLE)) return;
    if (svc_txframe_is_empty(&s_tx)) {
        s_tx_overflowed = false;            /* drained — re-arm the WARN */
        return;
    }
    uint16_t n = svc_txframe_pop(&s_tx, s_stage, sizeof s_stage);
    if (n) {
        (void)drv_rn4871_send(s_stage, n); /* tx_idle checked above -> accepted */
    }
}

static void rx_handler(const uint8_t *data, uint16_t len)
{
    /* drv_rn4871 has already stripped the module's %...% status tokens;
     * everything here is transparent payload. */
    for (uint16_t i = 0; i < len; i++) {
        svc_api_reassembler_feed_byte(API_TRANSPORT_BLE, &s_reasm, data[i]);
    }
}

static void send_via_ble(const uint8_t *data, uint16_t len, bool urgent)
{
    if (!svc_txframe_push(&s_tx, data, len, urgent)) {
        if (g_system_state.ble_tx_dropped_count < UINT16_MAX) {
            g_system_state.ble_tx_dropped_count++;
        }
        if (!s_tx_overflowed) {
            s_tx_overflowed = true;
            svc_log(API2_LOG_WARN, "ble: tx ring full, frame dropped");
        }
        return;
    }
    ble_tx_pump();
}

void svc_ble_init(void)
{
    s_was_connected = false;
    s_tx_overflowed = false;
    s_reasm.pos     = 0;
    svc_txframe_init(&s_tx, s_tx_buf, sizeof s_tx_buf);
    svc_api_register_transport(API_TRANSPORT_BLE, send_via_ble);
    drv_rn4871_register_rx_callback(rx_handler);
    (void)drv_rn4871_init();
}

bool svc_ble_is_connected(void)
{
    return drv_rn4871_is_connected();
}

void svc_ble_task(void)
{
    drv_rn4871_task();
    svc_api_reassembler_check_timeout(&s_reasm, API_RX_PACKET_TIMEOUT_MS);

    bool now = drv_rn4871_is_connected();
    if (now && !s_was_connected) {
        s_reasm.pos = 0;
        svc_txframe_reset(&s_tx);
        s_tx_overflowed = false;
        svc_api_connected(API_TRANSPORT_BLE);
        svc_log(API2_LOG_INFO, "ble: central connected");
    } else if (!now && s_was_connected) {
        svc_api_disconnected(API_TRANSPORT_BLE);
        svc_txframe_reset(&s_tx);   /* queued frames are for a gone central */
        s_tx_overflowed = false;
        svc_log(API2_LOG_INFO, "ble: central disconnected");
    }
    s_was_connected = now;

    g_system_state.ble_connected = now;

    ble_tx_pump();
}
