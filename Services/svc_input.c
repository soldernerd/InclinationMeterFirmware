#include "svc_input.h"
#include "drv_encoder.h"
#include "hal_gpio.h"
#include "pin_config.h"
#include "system_state.h"
#include <stdbool.h>

static bool s_sw1_pressed;
static bool s_sw2_pressed;

void svc_input_init(void)
{
    /* Seed from the actual pin level, not a hardcoded "not pressed" —
     * ENC_1SW/ENC_2SW are this device's Standby wake-up sources
     * (SYS_WKUP1/SYS_WKUP5, see pin_config.h), and waking via Standby is
     * a full MCU reset that re-runs this init. If the button that woke
     * the device is still physically held down at that point, seeding
     * "not pressed" would make the very first svc_input_update() poll
     * see a false rising edge and fire a spurious press event. */
    s_sw1_pressed = hal_gpio_get(ENC_1SW_PORT, ENC_1SW_PIN);
    s_sw2_pressed = hal_gpio_get(ENC_2SW_PORT, ENC_2SW_PIN);
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

    /* Latch press edges rather than overwrite — app_ui.c (or any future
     * consumer) runs slower than this polling task and must not miss a
     * press that happens between its ticks. Consumer clears the flag
     * after acting on it. */
    if (sw1_now && !s_sw1_pressed) {
        g_system_state.encoder1_sw_press_event = true;
    }
    if (sw2_now && !s_sw2_pressed) {
        g_system_state.encoder2_sw_press_event = true;
    }
    s_sw1_pressed = sw1_now;
    s_sw2_pressed = sw2_now;

    g_system_state.encoder1_count      = drv_encoder_get_count(ENCODER_1);
    g_system_state.encoder2_count      = drv_encoder_get_count(ENCODER_2);
    g_system_state.encoder1_sw_pressed = sw1_now;
    g_system_state.encoder2_sw_pressed = sw2_now;
}
