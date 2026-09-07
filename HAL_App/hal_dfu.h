#ifndef HAL_DFU_H
#define HAL_DFU_H

/* Reboot into the STM32G0 ROM system bootloader (USB DFU on PA11/PA12,
 * VID 0x0483 / PID 0xDF11) so the device can be reflashed with no ST-Link.
 *
 * Why the option byte and not a software jump: every software path into
 * the bootloader (in-app jump to 0x1FFF0000, or a clean jump from the top
 * of a fresh reset, or forcing FLASH PROGEMPTY) was tried on this board
 * (fw 0.4.20-0.4.36 and again on the `dfu` branch). The jump now reliably
 * REACHES the bootloader, but with a valid application present and no
 * hardware BOOT0 assertion the bootloader hands control straight back to
 * the app — even with flash mass-erased, because the "flash is empty"
 * check that would make it stay is only re-sampled at a power-on reset,
 * which firmware cannot produce. Bench-verified: nBOOT0 = 0 is the only
 * entry the bootloader treats as "stay here", and it brings up USB DFU
 * cleanly on this hardware.
 *
 * So this sets the nBOOT0 user option byte to 0 and launches an option-
 * byte reload (a reset that re-reads the boot configuration). The next
 * boot — and EVERY boot after it — goes to the ROM bootloader until the
 * option byte is set back to 1.
 *
 * RECOVERY IS NOT AUTOMATIC. A plain power-cycle does NOT bring the
 * application back; the device stays in DFU until a host reflashes it
 * AND restores nBOOT0 = 1 in the same operation, e.g.
 *
 *   STM32_Programmer_CLI -c port=USB1 -w firmware.hex \
 *       -ob nSWBOOT0=1 nBOOT0=1 -v -rst
 *
 * or run PythonTestCode/dfu_flash.ps1. See docs/wp4_reboot_to_dfu.md.
 */

/* Set nBOOT0 = 0 and launch the option-byte reload. Does not return on
 * success (the reload resets the MCU into the ROM bootloader). Returns
 * only if the option-byte programming failed, having issued a plain
 * NVIC_SystemReset() as a fallback so the device is never left in a
 * half-configured state. Call from a command / menu handler after the
 * response has been flushed to the transport. */
void hal_dfu_enter_bootloader(void);

#endif /* HAL_DFU_H */
