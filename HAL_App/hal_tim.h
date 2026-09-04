#ifndef HAL_TIM_H
#define HAL_TIM_H

#include <stdint.h>

void hal_tim_init(void);
void hal_tim_vcom_start(void);
void hal_tim_vcom_stop(void);

/* Buzzer — TIM3 CH4 on PC9, AF1 (REV B; the REV A prototype used TIM1 CH2
 * on PB3). Prescaler set to 63 in CubeMX so the timer ticks at 1 MHz;
 * ARR/CCR4 are programmed dynamically per call.
 *
 * hal_tim_buzzer_start()  — sound the tone until hal_tim_buzzer_stop().
 * hal_tim_buzzer_beep()   — sound the tone for exactly duration_ms, then
 *   stop it from the TIM3 update ISR. The length is counted in whole PWM
 *   periods by hardware, so it does NOT depend on scheduler / display-
 *   render timing (which is why the old polled stop gave uneven beeps).
 *   A second call while a beep is still sounding only ever lengthens it.
 * hal_tim_buzzer_isr()    — call from TIM3_TIM4_IRQHandler(). */
void hal_tim_buzzer_start(uint16_t freq_hz);
void hal_tim_buzzer_beep(uint16_t freq_hz, uint16_t duration_ms);
void hal_tim_buzzer_isr(void);
void hal_tim_buzzer_stop(void);

#endif /* HAL_TIM_H */
