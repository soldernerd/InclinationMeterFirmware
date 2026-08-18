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

/* ADS131M04 ADC master clock (WP8) — TIM2 CH3 on PB10. Same fixed
 * 64 MHz / (2 x 6) = 5.3333... MHz as the DAC's clock above (Prescaler=1,
 * Counter Period=5, Pulse=3) — see Config/config.h's ADS131M04_OSR_FIELD
 * comment for why the DAC and ADC deliberately share this clock
 * relationship. Started once, left running forever, same reasoning as
 * hal_tim_dac_clock_start(). */
void hal_tim_adc_clock_start(void);

typedef void (*HalTimCallback)(void);

/* ADC acquisition trigger (WP8) — TIM7, a basic timer with no GPIO
 * output. Interrupts at exactly 20833.33 Hz (Prescaler=0, Counter
 * Period=3071, CubeMX-configured — see Config/config.h's
 * ADS131M04_TRIGGER_TIMER_PERIOD), matching the ADC's own sample rate.
 * The registered callback runs in interrupt context — keep it short.
 * This indirection keeps HAL_App ignorant of what "ADC" means (it just
 * dispatches on which TIM instance fired, same pattern as
 * HAL_TIM_PeriodElapsedCallback's existing TIM6 case below) — the actual
 * per-sample logic belongs in Drivers_App/drv_ads131m04.c. */
void hal_tim_adc_trigger_start(void);
void hal_tim_adc_trigger_register_callback(HalTimCallback cb);

#endif /* HAL_TIM_H */
