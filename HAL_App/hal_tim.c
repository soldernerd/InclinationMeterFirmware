#include "hal_tim.h"
#include "stm32g0xx_hal.h"
#include "pin_config.h"

extern TIM_HandleTypeDef htim6;

void hal_tim_init(void)
{
    /* Nothing to pre-configure: TIM6 is a plain periodic base timer (no
     * channels) fully set up by MX_TIM6_Init(); the VCOM toggle itself
     * happens in HAL_TIM_PeriodElapsedCallback() below. */
}

void hal_tim_vcom_start(void)
{
    HAL_TIM_Base_Start_IT(&htim6);
}

void hal_tim_vcom_stop(void)
{
    HAL_TIM_Base_Stop_IT(&htim6);
}

void hal_tim_set_buzzer_freq(uint16_t freq_hz)
{
    /* WP1 stub — TIM3 buzzer implemented in WPx */
    (void)freq_hz;
}

void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
    if (htim->Instance == TIM6) {
        HAL_GPIO_TogglePin(DISP_VCOM_PORT, DISP_VCOM_PIN);
    }
}
