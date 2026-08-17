#include "svc_ble.h"
#include "svc_api.h"
#include "drv_rn4871.h"
#include "svc_storage.h"
#include "config.h"
#include "system_state.h"
#include "hal_systick.h"
#include <string.h>

/* BLE transport adapter. Owns:
 *   - registration with svc_api as API_TRANSPORT_BLE
 *   - drv_rn4871 connect/disconnect bridging into svc_api and
 *     g_system_state.ble_connected (the sole writer of that field —
 *     Drivers_App/drv_rn4871.c deliberately does not touch system_state)
 *   - reassembly of incoming bytes into 64-byte packets
 *
 * RN4871 in transparent UART mode forwards CMD-characteristic writes
 * from the BLE peer as raw bytes on UART. We accumulate them into a
 * packet buffer until LEN+overhead bytes are present, then dispatch.
 * If a partial packet stalls, BLE_RX_PACKET_TIMEOUT_MS abandons it. */

#define PKT_HDR     2U
#define PKT_CRC     2U
#define PKT_MAX     64U

static uint8_t  s_rx_buf[PKT_MAX];
static uint16_t s_rx_pos = 0;
static uint32_t s_rx_started_ms = 0;

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
    if (s_rx_pos == 0) {
        s_rx_started_ms = hal_systick_get_ms();
    }
    if (s_rx_pos < PKT_MAX) {
        s_rx_buf[s_rx_pos++] = b;
    }

    /* As soon as we have header + length, we know how big the packet is. */
    if (s_rx_pos >= PKT_HDR) {
        uint16_t paylen = s_rx_buf[1];
        uint16_t total  = (uint16_t)(PKT_HDR + paylen + PKT_CRC);
        if (total > PKT_MAX) {
            /* Garbage — drop. */
            s_rx_pos = 0;
        } else if (s_rx_pos >= total) {
            svc_api_receive(API_TRANSPORT_BLE, s_rx_buf, total);
            s_rx_pos = 0;
        }
    }
}

void svc_ble_on_connected(void)
{
    g_system_state.ble_connected = true;
    svc_api_connected(API_TRANSPORT_BLE);
    s_rx_pos = 0;
}

void svc_ble_on_disconnected(void)
{
    g_system_state.ble_connected = false;
    svc_api_disconnected(API_TRANSPORT_BLE);
    s_rx_pos = 0;
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
    s_rx_pos = 0;
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

    /* Abort partial packets that stalled */
    if (s_rx_pos > 0
        && (hal_systick_get_ms() - s_rx_started_ms) > BLE_RX_PACKET_TIMEOUT_MS) {
        s_rx_pos = 0;
    }
}
