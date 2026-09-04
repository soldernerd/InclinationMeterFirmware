#ifndef HAL_POWER_H
#define HAL_POWER_H

#include <stdbool.h>
#include <stdint.h>

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

/* Reboots into the STM32 ROM system bootloader (USB DFU / UART) instead
 * of this firmware — used by the SETTINGS screen's "Reboot to DFU" menu
 * action so the device can be field-updated over USB without an ST-LINK.
 * Stores a magic value in a .noinit RAM variable (survives
 * NVIC_SystemReset() — Reset_Handler's zero-fill loop skips that section;
 * see STM32G0B1XX_FLASH.ld) and calls NVIC_SystemReset(); does not
 * return. hal_power_check_dfu_request_and_jump() is what actually acts
 * on the request, very early on the next boot. */
void hal_power_request_dfu_reboot(void);

/* Call once, as the very first thing in main() — before any clock,
 * GPIO, or peripheral init. If the last reset was requested by
 * hal_power_request_dfu_reboot(), clears the magic value (so a later
 * normal reset doesn't loop back into DFU) and jumps into the ROM system
 * bootloader; does not return in that case. Otherwise returns
 * immediately and normal boot continues — the common case, and cheap
 * (a couple of RAM reads). */
void hal_power_check_dfu_request_and_jump(void);

/* The magic-value RAM location as last read by either
 * hal_power_request_dfu_reboot() (right after writing it) or
 * hal_power_check_dfu_request_and_jump() (at boot) — whichever ran most
 * recently in this session. Temporary bring-up visibility (rendered on
 * the STATUS screen). Name kept from an earlier TAMP-backup-register
 * version of this mechanism; no longer register-specific. */
uint32_t hal_power_last_bkp0r(void);

#endif /* HAL_POWER_H */
