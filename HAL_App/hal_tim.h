#ifndef HAL_TIM_H
#define HAL_TIM_H

#include <stdint.h>

void hal_tim_init(void);
void hal_tim_vcom_start(void);
void hal_tim_vcom_stop(void);

/* Buzzer — TIM3 CH4 on PC9, AF1 (REV B; the REV A prototype used TIM1 CH2
 * on PB3). Prescaler set to 63 in CubeMX so the timer ticks at 1 MHz;
 * ARR/CCR4 are programmed dynamically per call. */
void hal_tim_buzzer_start(uint16_t freq_hz);
void hal_tim_buzzer_stop(void);

/* AD9833 DAC master clock (WP7) — TIM1 CH4 on PC11. Fixed at
 * 64 MHz / (2 x 6) = 5.3333... MHz (Prescaler=1, Counter Period=5,
 * Pulse=3 for ~50% duty, all CubeMX-configured — see
 * Config/config.h's AD9833_FREQREG comment for why this exact
 * frequency was chosen). Started once and left running forever: unlike
 * the buzzer, there's no dynamic reconfiguration or stop — the AD9833
 * needs a continuous MCLK to keep generating its waveform for as long
 * as it's in use. */
void hal_tim_dac_clock_start(void);

#endif /* HAL_TIM_H */
