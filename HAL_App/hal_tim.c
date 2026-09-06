#include "hal_tim.h"
#include "stm32g0xx_hal.h"
#include "pin_config.h"

extern TIM_HandleTypeDef htim1;
extern TIM_HandleTypeDef htim2;
extern TIM_HandleTypeDef htim3;
extern TIM_HandleTypeDef htim6;
extern TIM_HandleTypeDef htim7;

static HalTimCallback s_adc_trigger_cb = 0;

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

/* Non-zero while a hal_tim_buzzer_beep() tone is sounding: the number of
 * whole PWM periods still owed before the ISR stops the output. Touched by
 * both thread code (under __disable_irq) and the TIM3 update ISR. */
static volatile uint32_t s_buzzer_periods_left = 0U;

/* Program ARR/CCR4 for freq_hz and force the shadow registers to load now.
 *
 * Timer clock = 1 MHz (prescaler = 63, CubeMX-configured in Core/Src/tim.c;
 * APB1 = HCLK = 64 MHz, confirmed unprescaled in SystemClock_Config()), so
 * one timer tick = 1 us. ARR = ticks per PWM period - 1; CCR4 = period/2
 * for ~50% duty. REV B moved the buzzer from the REV A prototype's
 * TIM1_CH2/PB3 to TIM3_CH4/PC9 — see pin_config.h's BUZZER_PIN.
 *
 * MX_TIM3_Init leaves ARR/CCR preload enabled (ARR=999 from CubeMX), so the
 * register writes only reach the shadows; without the forced update the
 * first period of every beep would run at the previous frequency (1 kHz on
 * the very first beep) until the counter next wrapped — an audible chunk of
 * a 20 ms beep. TIM_EGR_UG loads the shadows and re-zeros the counter; the
 * update flag it raises is cleared so the caller can decide whether the
 * period-counting interrupt should see subsequent updates. Returns the PWM
 * period length in timer ticks (= microseconds). */
static uint32_t buzzer_program_freq(uint16_t freq_hz)
{
    uint32_t arr = (1000000UL / freq_hz) - 1U;

    __HAL_TIM_SET_AUTORELOAD(&htim3, arr);
    __HAL_TIM_SET_COMPARE(&htim3, TIM_CHANNEL_4, (arr + 1U) / 2U);
    htim3.Instance->EGR = TIM_EGR_UG;
    __HAL_TIM_CLEAR_FLAG(&htim3, TIM_FLAG_UPDATE);
    __HAL_TIM_SET_COUNTER(&htim3, 0U);

    return arr + 1U;
}

void hal_tim_buzzer_start(uint16_t freq_hz)
{
    if (freq_hz == 0U) {
        hal_tim_buzzer_stop();
        return;
    }
    __HAL_TIM_DISABLE_IT(&htim3, TIM_IT_UPDATE);
    s_buzzer_periods_left = 0U;
    (void)buzzer_program_freq(freq_hz);
    HAL_TIM_PWM_Start(&htim3, TIM_CHANNEL_4);
}

void hal_tim_buzzer_beep(uint16_t freq_hz, uint16_t duration_ms)
{
    if (freq_hz == 0U || duration_ms == 0U) {
        hal_tim_buzzer_stop();
        return;
    }

    /* period length in us; periods needed for duration_ms, rounded to
     * nearest whole PWM period (2 kHz => 500 us => 40 periods for 20 ms). */
    uint32_t period_us = (1000000UL / freq_hz);
    uint32_t periods   = ((uint32_t)duration_ms * 1000UL + period_us / 2U) / period_us;
    if (periods == 0U) {
        periods = 1U;
    }

    uint32_t primask = __get_PRIMASK();
    __disable_irq();

    if (s_buzzer_periods_left != 0U) {
        /* A beep is already sounding — only ever lengthen it, never cut it
         * short. The tone and pitch are already correct; just top up the
         * period budget the ISR is counting down. */
        if (periods > s_buzzer_periods_left) {
            s_buzzer_periods_left = periods;
        }
        __set_PRIMASK(primask);
        return;
    }

    (void)buzzer_program_freq(freq_hz);
    s_buzzer_periods_left = periods;
    __HAL_TIM_ENABLE_IT(&htim3, TIM_IT_UPDATE);
    HAL_TIM_PWM_Start(&htim3, TIM_CHANNEL_4);

    __set_PRIMASK(primask);
}

/* TIM3 update ISR body — one call per elapsed PWM period. Counts the beep
 * down and cuts the output at exactly the requested number of periods,
 * independent of the main loop. */
void hal_tim_buzzer_isr(void)
{
    if (__HAL_TIM_GET_FLAG(&htim3, TIM_FLAG_UPDATE) == RESET) {
        return;
    }
    if (__HAL_TIM_GET_IT_SOURCE(&htim3, TIM_IT_UPDATE) == RESET) {
        /* Update flag is set but its interrupt isn't armed — not ours to
         * act on (e.g. a stale flag from buzzer_program_freq's UG). */
        return;
    }
    __HAL_TIM_CLEAR_FLAG(&htim3, TIM_FLAG_UPDATE);

    if (s_buzzer_periods_left != 0U) {
        if (--s_buzzer_periods_left == 0U) {
            __HAL_TIM_DISABLE_IT(&htim3, TIM_IT_UPDATE);
            HAL_TIM_PWM_Stop(&htim3, TIM_CHANNEL_4);
        }
    }
}

void hal_tim_buzzer_stop(void)
{
    __HAL_TIM_DISABLE_IT(&htim3, TIM_IT_UPDATE);
    s_buzzer_periods_left = 0U;
    HAL_TIM_PWM_Stop(&htim3, TIM_CHANNEL_4);
}

void hal_tim_dac_clock_start(void)
{
    /* Prescaler / Period / Pulse are fixed by MX_TIM1_Init() (Core/Src/tim.c)
     * at 1 / 5 / 3 — nothing to program per call, unlike the buzzer. Runs
     * forever afterward; see hal_tim.h. */
    HAL_TIM_PWM_Start(&htim1, TIM_CHANNEL_4);
}

void hal_tim_adc_clock_start(void)
{
    /* Same fixed 1/5/3 as the DAC's TIM1 above, on TIM2 CH3 instead
     * (MX_TIM2_Init()). Started once, left running. */
    HAL_TIM_PWM_Start(&htim2, TIM_CHANNEL_3);
}

void hal_tim_adc_trigger_start(void)
{
    HAL_TIM_Base_Start_IT(&htim7);
}
void hal_tim_adc_trigger_stop(void)
{
    HAL_TIM_Base_Stop_IT(&htim7);
}
/* Lean entry point called straight from TIM7_LPTIM2_IRQHandler's fast
 * path (Core/Src/stm32g0xx_it.c) — the UIF flag is already cleared by the
 * caller, this just fans out to the registered ADS DRDY-poll callback. */
void hal_tim_adc_trigger_isr(void)
{
    if (s_adc_trigger_cb) {
        s_adc_trigger_cb();
    }
}

void hal_tim_adc_trigger_register_callback(HalTimCallback cb)
{
    s_adc_trigger_cb = cb;
}

void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
    if (htim->Instance == TIM6) {
        HAL_GPIO_TogglePin(DISP_VCOM_PORT, DISP_VCOM_PIN);
    } else if (htim->Instance == TIM7) {
        if (s_adc_trigger_cb) {
            s_adc_trigger_cb();
        }
    }
}
