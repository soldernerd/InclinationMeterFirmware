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
bool         svc_battery_is_charging(void);

/* Manual override: enable the charger irrespective of Vbat / SOC for as
 * long as USB is present. The auto-policy deliberately leaves a near-full
 * LiPo alone (repeated top-offs shorten its life); this lets a user force
 * an overnight top-off anyway. Cleared automatically once the pack
 * reaches full (TP4056 STANDBY) or USB is unplugged — re-select the menu
 * item to arm it again. No-op with no USB connected. */
void         svc_battery_force_charge(void);
bool         svc_battery_is_force_charging(void);

/* Cancels an armed force_charge override immediately (the next
 * svc_battery_update() re-evaluates the normal SOC-threshold policy from
 * scratch). Added 2026-09-27 -- force_charge previously had no way back
 * off except reaching full or physically removing USB, discovered as a
 * real gap while bench-testing (a force-charge triggered for a quick test
 * left the board charging with no way to stop it short of unplugging the
 * cable). No-op if not currently force-charging. Does not affect the
 * normal (non-forced) auto-charge policy -- if Vbat is still below
 * battery_charge_start_mv, charging resumes on its own next tick, same
 * as if force_charge had never been armed. */
void         svc_battery_cancel_force_charge(void);

/* Hard override: forces charging OFF regardless of what would otherwise
 * enable it -- the auto SOC-threshold policy AND an armed force_charge
 * both included. Added 2026-09-29 at the user's explicit request, after
 * discovering svc_battery_cancel_force_charge() above has no effect on a
 * charge cycle the auto-policy started on its own (the common case, not a
 * force-charge) -- that command only ever cancelled a *forced* session,
 * so it silently did nothing against natural charging, which is exactly
 * the gap this closes. Setting the inhibit also clears any armed
 * force_charge (inhibit wins over force, not the other way round).
 * update_charge_enable() checks this FIRST, ahead of both other paths.
 * Deliberately does NOT self-clear on USB removal/reconnect or on
 * charge-complete -- unlike force_charge's one-shot "top off now"
 * semantics, this is a standing "leave charging off" instruction that
 * stays in effect until explicitly cancelled, so re-plugging USB doesn't
 * silently resume charging behind the user's back. No EEPROM persistence
 * -- resets to "not inhibited" on reboot, same as force_charge. */
void         svc_battery_set_charge_inhibit(bool inhibit);
bool         svc_battery_is_charge_inhibited(void);

/* Disables the LEDs and the 3.3V/5V rails, then enters STM32 Standby mode.
 * Does not return — Standby mode resets the MCU on wake (see
 * HAL_App/hal_power.h). Public so a later work package's user-initiated
 * power-off can call this too, not just the critical-battery path. */
void             svc_battery_enter_low_power(void);

#endif /* SVC_BATTERY_H */
