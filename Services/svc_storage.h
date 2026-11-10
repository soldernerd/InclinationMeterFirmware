#ifndef SVC_STORAGE_H
#define SVC_STORAGE_H

#include <stdint.h>
#include <stdbool.h>
#include "drv_common.h"
#include "system_state.h"

void      svc_storage_init(void);    /* called at boot — loads or initialises EEPROM */
void      svc_storage_update(void);  /* call every scheduler tick — drives non-blocking writes */

/* Queues a save of `settings` (see config.h's EEPROM_*_SETTINGS_ADDR) and returns at once; it never refuses because a save is
 * already running. svc_storage_update() writes, in the background, only the pages that differ from what the EEPROM holds, and
 * keeps going until none does, so several changes made in a row are coalesced. svc_storage_is_busy() is true until every
 * requested change is on the chip. `settings` must stay valid until then; every caller passes the persistent
 * &g_device_settings. DRV_ERR_COMM = the chip could not be read at boot, nothing is ever written to it. There is no
 * svc_storage_load_settings() -- svc_storage_init() loads each page directly. */
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

/* Resets EVERY setting and calibration to the compiled defaults (in RAM at once) and saves all pages.
 * Wipes the tilt calibrations. */
DrvStatus svc_storage_restore_defaults(void);

bool      svc_storage_is_busy(void); /* true while a requested save is not yet on the chip */

#endif /* SVC_STORAGE_H */
