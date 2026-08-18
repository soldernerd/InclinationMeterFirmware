#ifndef SVC_STORAGE_H
#define SVC_STORAGE_H

#include <stdint.h>
#include <stdbool.h>
#include "drv_common.h"
#include "system_state.h"

void      svc_storage_init(void);    /* called at boot — loads or initialises EEPROM */
void      svc_storage_update(void);  /* call every scheduler tick — drives non-blocking writes */

/* Saves every DeviceSettings page (see config.h's EEPROM_*_SETTINGS_ADDR)
 * in sequence, non-blocking — svc_storage_is_busy() reports true until
 * all pages have been written. `settings` must stay valid for the whole
 * multi-tick operation; the only caller (App/app_ui.c) always passes the
 * persistent &g_device_settings. There is no svc_storage_load_settings()
 * — nothing outside this module needs it; svc_storage_init() loads each
 * page directly. */
DrvStatus svc_storage_save_settings(const DeviceSettings *settings);

/* Clamps every EEPROM-backed divisor field (encoder_counts_per_detent,
 * vbat_scale_den, tmp236_seg1_den, tmp236_seg2_den) to its DEFAULT_* value
 * if zero. svc_storage_init() runs this after loading from EEPROM; any
 * other writer of DeviceSettings — currently only Services/svc_api.c's
 * SET_SETTINGS handler, applying an untrusted host payload — must run it
 * too before accepting the write, or a zero divisor from a malformed
 * payload silently deadens whatever consumer divides by it (App/app_ui.c,
 * Services/svc_battery.c, Drivers_App/drv_tmp236.c). */
void svc_storage_validate_settings(DeviceSettings *settings);

DrvStatus svc_storage_save_calibration(const CalibrationData *cal);
DrvStatus svc_storage_load_calibration(CalibrationData *cal);

/* Generic single-blob save/load, one header+CRC+version page at
 * `base_addr` -- the mechanism svc_storage_save_calibration()/
 * load_calibration() above are themselves built on (WP10). For any
 * small standalone calibration struct with its own dedicated EEPROM
 * page, not threaded through DeviceSettings' SettingsSection
 * mechanism -- see system_state.h's DisplacementSensorCal/
 * DisplacementSharedCal for the current example. save is non-blocking
 * (same s_pending state machine, drained via svc_storage_update());
 * load is blocking, for boot-time use only, same as
 * svc_storage_load_calibration(). */
DrvStatus svc_storage_save_blob(uint16_t base_addr, uint16_t version,
                                const void *data, uint16_t len);
DrvStatus svc_storage_load_blob(uint16_t base_addr, uint16_t version,
                                void *data, uint16_t len);

bool      svc_storage_is_busy(void); /* true while a write operation is in progress */

#endif /* SVC_STORAGE_H */
