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

bool      svc_storage_is_busy(void); /* true while a write operation is in progress */

/* "Reboot to DFU" persistence (App/app_ui.c's SETTINGS action requests
 * it; Core/Src/main.c checks it early in boot, right after EEPROM access
 * comes up). Backed by EEPROM rather than RAM or a backup register:
 * both of those turned out not to survive NVIC_SystemReset() on this
 * board (confirmed empirically — a verified write to each still read
 * back as cleared on the very next boot), whereas EEPROM survives any
 * reset or power cycle by construction. Both calls are blocking: the
 * request happens right before a reset anyway, and the check happens
 * before anything else in boot is running that could be delayed by it. */
void svc_storage_request_dfu_reboot(void);
bool svc_storage_check_and_clear_dfu_reboot_flag(void);

#endif /* SVC_STORAGE_H */
