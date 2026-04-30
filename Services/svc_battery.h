#ifndef SVC_BATTERY_H
#define SVC_BATTERY_H

#include <stdint.h>
#include <stdbool.h>

typedef enum {
    BATTERY_NORMAL = 0,
    BATTERY_LOW,            /* < battery_low_pct */
    BATTERY_CRITICAL,       /* < battery_critical_pct  */
    BATTERY_CHARGING,
    BATTERY_FULL,           /* charge complete (CHRG HIGH while USB present) */
} BatteryState;

void         svc_battery_init(void);
void         svc_battery_update(void);
BatteryState svc_battery_get_state(void);
uint8_t      svc_battery_get_soc_pct(void);
uint16_t     svc_battery_get_vbat_mv(void);
bool         svc_battery_is_usb_connected(void);
bool         svc_battery_is_charging(void);

#endif /* SVC_BATTERY_H */
