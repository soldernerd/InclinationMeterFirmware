#include "drv_buzzer.h"
#include "hal_tim.h"

/* Beep timing lives in hardware now: hal_tim_buzzer_beep() programs TIM3
 * to sound the tone and the TIM3 update ISR (hal_tim_buzzer_isr) counts
 * whole PWM periods and stops the output at exactly the requested
 * duration. That makes every beep the same length regardless of what the
 * cooperative scheduler is doing — the old software-polled stop in
 * drv_buzzer_update() was at the mercy of the display render blocking the
 * main loop, which is why some beeps came out short and some long. */

void drv_buzzer_init(void)
{
    hal_tim_buzzer_stop();
}

void drv_buzzer_on(BuzzerTone tone)
{
    hal_tim_buzzer_start((uint16_t)tone);
}

void drv_buzzer_off(void)
{
    hal_tim_buzzer_stop();
}

void drv_buzzer_beep(BuzzerTone tone, uint16_t duration_ms)
{
    /* Fire-and-forget. A second call while a beep is still sounding only
     * ever lengthens it (see hal_tim_buzzer_beep). */
    hal_tim_buzzer_beep((uint16_t)tone, duration_ms);
}

void drv_buzzer_update(void)
{
    /* Nothing to do — beep termination is handled in the TIM3 ISR. Kept
     * as a no-op so the scheduler task table doesn't have to change. */
}
