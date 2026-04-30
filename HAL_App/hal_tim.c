#include "hal_tim.h"
#include "stm32g0xx_hal.h"

extern TIM_HandleTypeDef htim3;

void hal_tim_init(void)
{
    /* htim3 already initialised by MX_TIM3_Init().
     * CubeMX leaves CCR1 at 0 (0% duty); set 50% for VCOM square wave. */
    __HAL_TIM_SET_COMPARE(&htim3, TIM_CHANNEL_1, htim3.Init.Period / 2U);
}

void hal_tim_vcom_start(void)
{
    HAL_TIM_PWM_Start(&htim3, TIM_CHANNEL_1);
}

void hal_tim_vcom_stop(void)
{
    HAL_TIM_PWM_Stop(&htim3, TIM_CHANNEL_1);
}

void hal_tim_set_buzzer_freq(uint16_t freq_hz)
{
    /* WP1 stub — TIM1 buzzer implemented in WPx */
    (void)freq_hz;
}
