#include "svc_ble.h"
#include "svc_api.h"
#include "drv_rn4871.h"
#include "svc_storage.h"
#include "config.h"
#include "system_state.h"

/* BLE transport adapter. Owns:
 *   - registration with svc_api as API_TRANSPORT_BLE
 *   - drv_rn4871 connect/disconnect bridging into svc_api and
 *     g_system_state.ble_connected (the sole writer of that field —
 *     Drivers_App/drv_rn4871.c deliberately does not touch system_state)
 *   - reassembly of incoming bytes into packets, via svc_api.c's shared
 *     ApiByteReassembler (also used by Services/svc_uart.c — the framing
 *     is identical for any transport that delivers raw bytes instead of
 *     whole frames)
 *
 * RN4871 in transparent UART mode forwards CMD-characteristic writes
 * from the BLE peer as raw bytes on UART. If a partial packet stalls,
 * API_RX_PACKET_TIMEOUT_MS abandons it. */

static ApiByteReassembler s_reassembler;

/* CLAUDE.md 7.6 — No Silent Failures: drv_rn4871_send_notification() can
 * fail (not connected, or the UART TX DMA already busy) and there's no
 * retry queue, so a failed send here is genuinely lost. Escalate to
 * g_system_state.ble_tx_dropped_count rather than silently discarding
 * the result — mirrors Services/svc_usb.c's send_via_usb(). */
static void send_via_ble(const uint8_t *data, uint16_t len)
{
    if (drv_rn4871_send_notification(data, len) != DRV_OK) {
        if (g_system_state.ble_tx_dropped_count < UINT16_MAX) {
            g_system_state.ble_tx_dropped_count++;
        }
    }
}

static void on_data_byte(uint8_t b)
{
    svc_api_reassembler_feed_byte(API_TRANSPORT_BLE, &s_reassembler, b);
}

void svc_ble_on_connected(void)
{
    g_system_state.ble_connected = true;
    svc_api_connected(API_TRANSPORT_BLE);
    s_reassembler.pos = 0;
}

void svc_ble_on_disconnected(void)
{
    g_system_state.ble_connected = false;
    svc_api_disconnected(API_TRANSPORT_BLE);
    s_reassembler.pos = 0;
}

/* Fires once, only on the full (first-boot) config path, right after the
 * RN4871 confirms it's advertising with the new config applied. Owns
 * persisting ble_configured — the driver itself must not touch
 * g_device_settings/svc_storage (Drivers_App layering, see drv_rn4871.h).
 * Mirrors App/app_ui.c's commit_edit()/invoke_action() escalation pattern
 * (CLAUDE.md 7.6: no silent failures). */
static void on_config_complete(void)
{
    g_device_settings.ble_configured = true;
    if (svc_storage_save_settings(&g_device_settings) != DRV_OK) {
        g_system_state.settings_save_failed = true;
    }
}

void svc_ble_init(void)
{
    s_reassembler.pos = 0;
    g_system_state.ble_connected = false;

    svc_api_register_transport(API_TRANSPORT_BLE, send_via_ble);
    drv_rn4871_set_on_connect(svc_ble_on_connected);
    drv_rn4871_set_on_disconnect(svc_ble_on_disconnected);
    drv_rn4871_set_on_config_complete(on_config_complete);
    drv_rn4871_set_on_data(on_data_byte);

    drv_rn4871_init(g_device_settings.ble_configured);
}

bool svc_ble_is_connected(void)
{
    return drv_rn4871_is_connected();
}

Rn4871State svc_ble_get_state(void)
{
    return drv_rn4871_get_state();
}

void svc_ble_update(void)
{
    drv_rn4871_task();
    svc_api_reassembler_check_timeout(&s_reassembler, API_RX_PACKET_TIMEOUT_MS);
}
