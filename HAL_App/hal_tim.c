#include "hal_tim.h"
#include "stm32g0xx_hal.h"
#include "pin_config.h"

extern TIM_HandleTypeDef htim3;
extern TIM_HandleTypeDef htim6;

void hal_tim_init(void)
{
    /* TIM6 (display VCOM) is a plain periodic base timer (no channels),
     * fully set up by MX_TIM6_Init(); the VCOM toggle itself happens in
     * HAL_TIM_PeriodElapsedCallback() below. REV B has no hardware PWM
     * channel for VCOM (unlike the REV A prototype, which drove it via
     * TIM3_CH1) — see pin_config.h's DISP_VCOM_PIN comment.
     *
     * TIM3 (buzzer) is initialised by MX_TIM3_Init(); its ARR/CCR4 are
     * programmed dynamically per call by hal_tim_buzzer_start() below,
     * nothing to pre-configure here. */
}

void hal_tim_vcom_start(void)
{
    HAL_TIM_Base_Start_IT(&htim6);
}

void hal_tim_vcom_stop(void)
{
    HAL_TIM_Base_Stop_IT(&htim6);
}

void hal_tim_buzzer_start(uint16_t freq_hz)
{
    if (freq_hz == 0U) {
        hal_tim_buzzer_stop();
        return;
    }
    /* Timer clock = 1 MHz (prescaler = 63, CubeMX-configured in
     * Core/Src/tim.c; APB1 = HCLK = 64 MHz, confirmed unprescaled in
     * SystemClock_Config()). ARR = ticks per period - 1; CCR4 = ARR/2 for
     * ~50% duty. REV B moved the buzzer from the REV A prototype's
     * TIM1_CH2/PB3 to TIM3_CH4/PC9 — see pin_config.h's BUZZER_PIN. */
    uint32_t arr = (1000000UL / freq_hz) - 1U;

    __HAL_TIM_SET_AUTORELOAD(&htim3, arr);
    __HAL_TIM_SET_COMPARE(&htim3, TIM_CHANNEL_4, (arr + 1U) / 2U);
    /* MX_TIM3_Init leaves ARR/CCR preload enabled (ARR=999 from CubeMX), so
     * the writes above only reach the shadow registers — without forcing an
     * update the first period of every beep would run at the *previous*
     * frequency (1 kHz on the very first beep, or the last beep's tone
     * after that) until the counter next wraps. For a 20 ms beep that stale
     * first period is a large, audible chunk and reads as "the beep barely
     * sounded". TIM_EGR_UG loads the shadows into the active registers now
     * and re-zeros the counter, so the whole beep is at the right pitch. */
    htim3.Instance->EGR = TIM_EGR_UG;
    __HAL_TIM_SET_COUNTER(&htim3, 0U);

    HAL_TIM_PWM_Start(&htim3, TIM_CHANNEL_4);
}

void hal_tim_buzzer_stop(void)
{
    HAL_TIM_PWM_Stop(&htim3, TIM_CHANNEL_4);
}

void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
    if (htim->Instance == TIM6) {
        HAL_GPIO_TogglePin(DISP_VCOM_PORT, DISP_VCOM_PIN);
    }
}
