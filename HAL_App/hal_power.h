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
 * of continuing this firmware — App/app_ui.c's "Reboot to DFU" menu
 * action calls this directly. A direct software jump into System Memory
 * (SYSCFG remap + manual SP/PC load from the bootloader's own vector
 * table) — the third mechanism tried; see the .c file's comment for the
 * full history of why the other two (a jump missing a memory barrier,
 * then forcing FLASH_ACR.PROGEMPTY and reaching it via various kinds of
 * reset) didn't work. Does not erase or otherwise touch the application
 * in flash. The jump has not stuck on this hardware to date — the ROM
 * bootloader bounces straight back to the app — so on fall-through this
 * issues NVIC_SystemReset() to return the device to a working state
 * rather than hang. Reach DFU for real by power-cycling with BOOT0
 * asserted. Never returns. */
void hal_power_reboot_to_dfu(void);

/* Current commanded state of the two switched rails, read back from the
 * enable pins (PWR_3V3_EN active-LOW, PWR_5V_EN active-HIGH). Reflects
 * what firmware last drove, not a physical rail-voltage sense (there is
 * none). Both are on for the whole normal run — only Standby turns them
 * off, and nothing polls this there. For the API `Topic groups` status
 * stream. */
bool hal_power_rail_3v3_on(void);
bool hal_power_rail_5v_on(void);

#endif /* HAL_POWER_H */
