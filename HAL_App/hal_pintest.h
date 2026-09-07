#ifndef HAL_PINTEST_H
#define HAL_PINTEST_H

#include <stdint.h>
#include <stdbool.h>

/* Diagnostic: statically drive the six MCU->level-converter signals that
 * feed the display + buzzer, to hunt a suspected short in the 5V level
 * converter by watching the supply current per input pattern.
 *
 * Signals (pattern bit -> pin):
 *   bit0 DISP_SCK  PD1     bit3 DISP_ON   PD2
 *   bit1 DISP_MOSI PD4     bit4 DISP_VCOM PD3
 *   bit2 DISP_CS   PD0     bit5 BUZZER    PC9
 *
 * Arming tears down SPI2 (display), TIM6 (VCOM toggle) and TIM3_CH4
 * (buzzer PWM) and reconfigures all six as plain push-pull outputs. The
 * only way back to normal operation is a reboot (NVIC_SystemReset) — the
 * display/comms state left behind is not restorable in place. */

/* Arm once (idempotent) and drive `pattern` bits [5:0] onto the six pins.
 * If `allow_disp_on` is false, DISP_ON (bit3) is forced LOW regardless of
 * the pattern bit — VCOM must never sit static while the panel is
 * powered (permanent damage). Pass true only with the panel unplugged. */
void hal_pintest_apply(uint8_t pattern, bool allow_disp_on);

#endif /* HAL_PINTEST_H */
