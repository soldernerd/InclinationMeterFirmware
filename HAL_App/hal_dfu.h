#ifndef HAL_DFU_H
#define HAL_DFU_H

#include <stdbool.h>

/* Reboot into the STM32G0 ROM system bootloader (USB DFU on PA11/PA12,
 * plus USART/I2C/SPI) so the device can be reflashed with no ST-Link.
 *
 * History: five in-application "jump straight to system memory" variants
 * and two "force PROGEMPTY + reset" variants (fw 0.4.20-0.4.36, parked)
 * all failed — every one of them tore down a *running* system (RCC, NVIC,
 * USB, CRS) by hand and jumped from an unclean state. This module takes
 * the other route: set a retained flag, NVIC_SystemReset(), and perform
 * the jump as the very first thing in main() — before HAL_Init(), while
 * the core is still in its reset state (HSISYS clock, every peripheral in
 * reset, no IRQs, SysTick off). That is the environment the ROM
 * bootloader expects, and nothing needs de-initialising.
 *
 * The request flag lives in the .noinit RAM section (see the linker
 * script): it survives NVIC_SystemReset() but not a power-cycle / BOR /
 * Standby exit, so the DFU entry is strictly one-shot — if the host never
 * reflashes, the next reset boots the application normally. */

/* Set the one-shot request flag and immediately NVIC_SystemReset().
 * Does not return. Call from a command/menu handler after the response
 * has been flushed to the transport. */
void hal_dfu_request_and_reset(void);

/* Call as the first statement of main() (USER CODE BEGIN 1), before
 * HAL_Init(). If a DFU request is pending, clears it and jumps to the
 * ROM bootloader (does not return). Otherwise returns immediately and
 * boot continues as normal. */
void hal_dfu_check_and_jump(void);

/* One-shot: true exactly once after a boot whose immediately preceding
 * attempt jumped to the ROM bootloader but the bootloader handed control
 * straight back (fall-through past the jump). Clears itself on read.
 * Lets a normal-path log line record that the software DFU jump bounced.
 * Call after svc_log is up. */
bool hal_dfu_consume_bounce_flag(void);

#endif /* HAL_DFU_H */
