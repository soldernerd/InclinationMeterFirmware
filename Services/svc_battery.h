#ifndef SVC_BATTERY_H
#define SVC_BATTERY_H

#include <stdint.h>
#include <stdbool.h>

typedef enum {
    BATTERY_NORMAL = 0,
    BATTERY_LOW,            /* < battery_low_mv (and >= battery_critical_mv) */
    BATTERY_CRITICAL,       /* < battery_critical_mv */
    BATTERY_CHARGING,
    BATTERY_FULL,           /* charge complete — STANDBY_SENSE asserted while USB present */
} battery_state_t;

void             svc_battery_init(void);
void             svc_battery_update(void);
battery_state_t  svc_battery_get_state(void);
uint8_t      svc_battery_get_soc_pct(void);
uint16_t     svc_battery_get_vbat_mv(void);
bool         svc_battery_is_usb_connected(void);
bool         svc_battery_is_charging(void);

/* Manual override: enable the charger irrespective of Vbat / SOC for as
 * long as USB is present. The auto-policy deliberately leaves a near-full
 * LiPo alone (repeated top-offs shorten its life); this lets a user force
 * an overnight top-off anyway. Cleared automatically once the pack
 * reaches full (TP4056 STANDBY) or USB is unplugged — re-select the menu
 * item to arm it again. No-op with no USB connected. */
void         svc_battery_force_charge(void);
bool         svc_battery_is_force_charging(void);

/* Disables the LEDs and the 3.3V/5V rails, then enters STM32 Standby mode.
 * Does not return — Standby mode resets the MCU on wake (see
 * HAL_App/hal_power.h). Public so a later work package's user-initiated
 * power-off can call this too, not just the critical-battery path. */
void             svc_battery_enter_low_power(void);

#endif /* SVC_BATTERY_H */
