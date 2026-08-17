#include "drv_buzzer.h"
#include "hal_tim.h"
#include "hal_systick.h"
#include <stdbool.h>

static volatile bool     s_active         = false;
static          uint32_t s_beep_start_ms  = 0;
static          uint16_t s_beep_duration_ms = 0;

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
    drv_buzzer_on(tone);
    s_beep_start_ms    = hal_systick_get_ms();
    s_beep_duration_ms = duration_ms;
}

void drv_buzzer_update(void)
{
    if (!s_active) {
        return;
    }
    if (hal_systick_elapsed_ms(s_beep_start_ms) >= s_beep_duration_ms) {
        drv_buzzer_off();
    }
}
