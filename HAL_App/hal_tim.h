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

/* AD9833 DAC master clock (WP7) — TIM1 CH4 on PC11, fixed
 * 64 MHz / (2 x 6) = 5.3333... MHz (Prescaler 1 / Period 5 / Pulse 3,
 * CubeMX-configured in Core/Src/tim.c; see Config/config.h's
 * AD9833_FREQREG comment for why this exact frequency). Started once at
 * boot by drv_ad9833_init() and left running forever — the AD9833 needs
 * a continuous MCLK to keep generating its waveform. No stop / no
 * dynamic reconfig, unlike the buzzer. */
void hal_tim_dac_clock_start(void);

/* ADS131M04 ADC master clock (WP8) — TIM2 CH3 on PB10, same fixed
 * ~5.333 MHz as the DAC clock (Prescaler 1 / Period 5 / Pulse 3). Started
 * once, left running — see Config/config.h's ADS131M04_OSR_FIELD comment
 * for why the DAC and ADC share this clock relationship. */
void hal_tim_adc_clock_start(void);

typedef void (*HalTimCallback)(void);

/* ADC acquisition trigger (WP8) — TIM7, a basic timer with no GPIO
 * output, interrupting at 20833.33 Hz (Prescaler 0 / Period 3071) =
 * the ADC's own sample rate. The registered callback runs in interrupt
 * context — keep it short. HAL_App stays ignorant of what "ADC" means;
 * the per-sample logic lives in Drivers_App/drv_ads131m04.c.
 *
 * The trigger is NOT started at boot (WP8, v0.8.2): the 20833 Hz stream
 * has no consumer yet and running it unconditionally starved the
 * scheduler's SysTick. drv_ads131m04_start()/stop() gate it at runtime
 * (toggled over the API — API2_RES_CMD_SIGNAL_ANALYSIS); _stop() calls
 * hal_tim_adc_trigger_stop(). */
void hal_tim_adc_trigger_start(void);
void hal_tim_adc_trigger_stop(void);
void hal_tim_adc_trigger_register_callback(HalTimCallback cb);

/* Called from the lean TIM7 ISR fast path in Core/Src/stm32g0xx_it.c
 * (UIF already cleared there). Not for application use. */
void hal_tim_adc_trigger_isr(void);

#endif /* HAL_TIM_H */
