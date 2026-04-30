#ifndef HAL_TIM_H
#define HAL_TIM_H

#include <stdint.h>

void hal_tim_init(void);
void hal_tim_vcom_start(void);
void hal_tim_vcom_stop(void);
void hal_tim_set_buzzer_freq(uint16_t freq_hz);

#endif /* HAL_TIM_H */
