#include "svc_input.h"
#include "drv_encoder.h"
#include "drv_buzzer.h"
#include "hal_gpio.h"
#include "pin_config.h"
#include "system_state.h"
#include <stdint.h>
#include <stdbool.h>

#define BUTTON_BEEP_MS   30U

static bool    s_sw1_pressed;
static bool    s_sw2_pressed;
static int32_t s_enc1_last_count;
static int32_t s_enc2_last_count;

void svc_input_init(void)
{
    s_sw1_pressed     = false;
    s_sw2_pressed     = false;
    s_enc1_last_count = drv_encoder_get_count(ENCODER_1);
    s_enc2_last_count = drv_encoder_get_count(ENCODER_2);
}

void svc_input_update(void)
{
    /* ENC_1SW/ENC_2SW aren't EXTI-capable on this pinout (both would-be
     * lines are already owned by the encoder A/B signals — see
     * pin_config.h), so they're polled here rather than interrupt-driven.
     * Both pass through an RC filter + 74HC14 Schmitt-trigger *inverter*,
     * so the MCU pin reads HIGH while pressed — no further debounce
     * needed (CLAUDE.md's hardware debounce note). */
    bool sw1_now = hal_gpio_get(ENC_1SW_PORT, ENC_1SW_PIN);
    bool sw2_now = hal_gpio_get(ENC_2SW_PORT, ENC_2SW_PIN);

    if (sw1_now && !s_sw1_pressed) {
        drv_buzzer_beep(BUZZER_TONE_CLICK, BUTTON_BEEP_MS);
    }
    if (sw2_now && !s_sw2_pressed) {
        drv_buzzer_beep(BUZZER_TONE_CLICK, BUTTON_BEEP_MS);
    }
    s_sw1_pressed = sw1_now;
    s_sw2_pressed = sw2_now;

    int32_t enc1_count = drv_encoder_get_count(ENCODER_1);
    int32_t enc2_count = drv_encoder_get_count(ENCODER_2);

    if (enc1_count != s_enc1_last_count) {
        drv_buzzer_beep(BUZZER_TONE_CLICK, BUTTON_BEEP_MS);
        s_enc1_last_count = enc1_count;
    }
    if (enc2_count != s_enc2_last_count) {
        drv_buzzer_beep(BUZZER_TONE_CLICK, BUTTON_BEEP_MS);
        s_enc2_last_count = enc2_count;
    }

    g_system_state.encoder1_count      = enc1_count;
    g_system_state.encoder2_count      = enc2_count;
    g_system_state.encoder1_sw_pressed = sw1_now;
    g_system_state.encoder2_sw_pressed = sw2_now;
}
