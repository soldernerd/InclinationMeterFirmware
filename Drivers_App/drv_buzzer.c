#include "drv_buzzer.h"
#include "hal_tim.h"
#include "hal_systick.h"
#include <stdbool.h>

static volatile bool s_active            = false;
static uint32_t      s_beep_deadline_ms  = 0;   /* absolute stop time */

void drv_buzzer_init(void)
{
    s_active = false;
    hal_tim_buzzer_stop();
}

void drv_buzzer_on(BuzzerTone tone)
{
    hal_tim_buzzer_start((uint16_t)tone);
    s_active = true;
}

void drv_buzzer_off(void)
{
    hal_tim_buzzer_stop();
    s_active = false;
}

void drv_buzzer_beep(BuzzerTone tone, uint16_t duration_ms)
{
    uint32_t deadline = hal_systick_get_ms() + duration_ms;

    if (!s_active) {
        drv_buzzer_on(tone);
        s_beep_deadline_ms = deadline;
        return;
    }

    /* A beep is already sounding — only ever push the stop time LATER,
     * never earlier. Rapid UI events (a press beep followed by a rotation
     * beep, or a fast encoder spin) then blend into one continuous click
     * instead of chopping the first one short. */
    if ((int32_t)(deadline - s_beep_deadline_ms) > 0) {
        s_beep_deadline_ms = deadline;
    }
}

void drv_buzzer_update(void)
{
    if (s_active && (int32_t)(hal_systick_get_ms() - s_beep_deadline_ms) >= 0) {
        drv_buzzer_off();
    }
}
