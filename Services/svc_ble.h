#ifndef SVC_BLE_H
#define SVC_BLE_H

#include <stdint.h>
#include <stdbool.h>
#include "drv_rn4871.h"

void svc_ble_init(void);
void svc_ble_update(void);
bool svc_ble_is_connected(void);

/* Passthrough to drv_rn4871_get_state() — App/app_display.c goes through
 * here rather than including Drivers_App/drv_rn4871.h itself (CLAUDE.md
 * 8.1: App must go through Services, not reach into Drivers_App). */
Rn4871State svc_ble_get_state(void);

void svc_ble_on_connected(void);
void svc_ble_on_disconnected(void);

#endif /* SVC_BLE_H */
