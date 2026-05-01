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

#endif /* HAL_TIM_H */
