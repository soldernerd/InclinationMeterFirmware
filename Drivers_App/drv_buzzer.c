#include "drv_buzzer.h"
#include "hal_tim.h"
#include "hal_systick.h"
#include <stdbool.h>

/* Hard floor on how short a beep can physically be, independent of what
 * duration the caller asked for or how the deadline math works out. The
 * merge logic below already guarantees at least the requested duration
 * from the last drv_buzzer_beep() call, but this is the backstop that
 * makes "the beep sometimes lasts only a few ms" structurally impossible:
 * once the tone is switched on it stays on for at least this long. */
#define BUZZER_MIN_ON_MS  15U

static volatile bool s_active            = false;
static uint32_t      s_beep_start_ms     = 0;   /* when the tone came on   */
static uint32_t      s_beep_deadline_ms  = 0;   /* absolute stop time      */

void drv_buzzer_init(void)
{
    s_active = false;
    hal_tim_buzzer_stop();
}

void drv_buzzer_on(BuzzerTone tone)
{
    hal_tim_buzzer_start((uint16_t)tone);
    s_beep_start_ms = hal_systick_get_ms();
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
    if (!s_active) {
        return;
    }
    uint32_t now = hal_systick_get_ms();

    /* Not past the requested stop time yet. */
    if ((int32_t)(now - s_beep_deadline_ms) < 0) {
        return;
    }
    /* Past the stop time, but hold the tone until the minimum on-time has
     * elapsed so a beep is never cut down to a click. */
    if ((int32_t)(now - s_beep_start_ms) < (int32_t)BUZZER_MIN_ON_MS) {
        return;
    }
    drv_buzzer_off();
}
