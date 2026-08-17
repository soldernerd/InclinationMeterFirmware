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

DrvStatus svc_storage_save_calibration(const CalibrationData *cal);
DrvStatus svc_storage_load_calibration(CalibrationData *cal);

bool      svc_storage_is_busy(void); /* true while a write operation is in progress */

#endif /* SVC_STORAGE_H */
