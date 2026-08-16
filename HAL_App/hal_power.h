#ifndef HAL_POWER_H
#define HAL_POWER_H

#include <stdbool.h>

/* Configures the 3 wake-up pins that can bring the MCU out of Standby
 * mode — ENC_1SW (WKUP1), VBUS_SENSE (WKUP4), ENC_2SW (WKUP5), all
 * high-level triggered. Call once, any time before hal_power_enter_standby(). */
void hal_power_configure_wakeup_pins(void);

/* Configures Standby-mode I/O retention for the two power-rail-enable
 * pins. REV B hardware gap (confirmed against the schematic, 2026-08-17):
 * PWR_3V3_EN (PC6) drives a P-MOSFET gate directly with no external
 * pull-up, and PWR_5V_EN (PC7) feeds an active-low shutdown input
 * ("must not be allowed to float" per its datasheet) with no external
 * pull-down. Standby mode powers down the whole GPIO configuration
 * domain, so without this, both pins would float to an undefined state
 * the instant Standby actually engages — regardless of what level
 * firmware last drove them to. This holds PC6 high (P-MOSFET off, rail
 * off) and PC7 low (shutdown asserted, rail off) using the MCU's own weak
 * internal pulls, retained through Standby via the PWR peripheral's
 * PUCR/PDCR + APC mechanism. Call once before hal_power_enter_standby(). */
void hal_power_configure_rail_retention(void);

/* True if this boot resumed from a Standby-mode wake (the PWR_FLAG_SB
 * flag was set at reset) rather than a power-on or other reset. Clears
 * PWR_FLAG_SB and all PWR_FLAG_WUFx flags as a side effect — call once,
 * early in boot. */
bool hal_power_woke_from_standby(void);

/* Enters STM32 Standby mode (~0.28 uA). Does not return: Standby mode
 * always resets the MCU on wake, so execution resumes at the reset
 * vector, same as a power-on reset, not back here. Caller is responsible
 * for disabling whatever rails/peripherals should be off first. */
void hal_power_enter_standby(void);

#endif /* HAL_POWER_H */
