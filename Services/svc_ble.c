#include "svc_ble.h"
#include "svc_api.h"
#include "svc_log.h"
#include "drv_rn4871.h"
#include "config.h"
#include "system_state.h"

/* BLE transport for svc_api (API v2), mirroring svc_usb.c. The RN4871
 * Transparent UART is a raw byte stream, so inbound bytes are fed into
 * svc_api's shared ApiByteReassembler to rebuild
 * [OPCODE][LEN][PAYLOAD][CRC16] packets before dispatch. Outbound,
 * svc_api already builds an exact 6+LEN packet (v2 has no padding), so
 * send_via_ble() just forwards it. The RN4871 config/reset state machine
 * is pumped from drv_rn4871_task() inside svc_ble_task(); nothing blocks. */

static ApiByteReassembler s_reasm;

static void rx_handler(const uint8_t *data, uint16_t len)
{
    /* drv_rn4871 has already stripped the module's %...% status tokens;
     * everything here is transparent payload. */
    for (uint16_t i = 0; i < len; i++) {
        svc_api_reassembler_feed_byte(API_TRANSPORT_BLE, &s_reasm, data[i]);
    }
}

static void send_via_ble(const uint8_t *data, uint16_t len)
{
    if (drv_rn4871_send(data, len) != DRV_OK) {
        if (g_system_state.ble_tx_dropped_count < UINT16_MAX) {
            g_system_state.ble_tx_dropped_count++;
        }
    }
}

static bool s_was_connected;

void svc_ble_init(void)
{
    s_was_connected = false;
    s_reasm.pos = 0;
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
        svc_api_connected(API_TRANSPORT_BLE);
        svc_log(API2_LOG_INFO, "ble: central connected");
    } else if (!now && s_was_connected) {
        svc_api_disconnected(API_TRANSPORT_BLE);
        svc_log(API2_LOG_INFO, "ble: central disconnected");
    }
    s_was_connected = now;

    g_system_state.ble_connected = now;
}
