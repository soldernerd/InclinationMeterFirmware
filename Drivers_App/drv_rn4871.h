#ifndef DRV_RN4871_H
#define DRV_RN4871_H

#include <stdint.h>
#include <stdbool.h>
#include "drv_common.h"

typedef enum {
    RN4871_STATE_BOOTING = 0,
    RN4871_STATE_ENTERING_CMD,
    RN4871_STATE_CONFIGURING,
    RN4871_STATE_REBOOTING,
    RN4871_STATE_RE_ENTERING_CMD,
    RN4871_STATE_ADVERTISING,
    RN4871_STATE_CONNECTED,
    RN4871_STATE_ERROR,
} Rn4871State;

/* already_configured: pass g_device_settings.ble_configured — this driver
 * doesn't read system_state/EEPROM itself (Drivers_App must not reach into
 * Services, see CLAUDE.md 8.1); the caller (Services/svc_ble.c) owns that. */
void          drv_rn4871_init(bool already_configured);
void          drv_rn4871_task(void);
Rn4871State   drv_rn4871_get_state(void);
bool          drv_rn4871_is_connected(void);
DrvStatus     drv_rn4871_send_notification(const uint8_t *data, uint16_t len);

/* Hooks fired by the driver when BLE peer state changes — svc_ble
 * registers these via the corresponding setters. */
typedef void (*Rn4871EventCb)(void);
void drv_rn4871_set_on_connect(Rn4871EventCb cb);
void drv_rn4871_set_on_disconnect(Rn4871EventCb cb);

/* Fired once, only on the full (first-boot) configuration path, right
 * after the module confirms it's advertising with the new config applied.
 * svc_ble.c is the one that persists g_device_settings.ble_configured and
 * saves it to EEPROM (Services owns system_state/svc_storage — this
 * driver must not, same layering reason as above). */
void drv_rn4871_set_on_config_complete(Rn4871EventCb cb);

/* Drains a received-data byte into the consumer (svc_ble). The driver
 * calls this for every byte arriving in CONNECTED state, after stripping
 * any unsolicited event lines. */
typedef void (*Rn4871DataCb)(uint8_t byte);
void drv_rn4871_set_on_data(Rn4871DataCb cb);

#endif /* DRV_RN4871_H */
