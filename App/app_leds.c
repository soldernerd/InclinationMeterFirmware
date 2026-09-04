#include "app_leds.h"
#include "hal_gpio.h"
#include "hal_systick.h"
#include "pin_config.h"
#include "system_state.h"
#include <stdbool.h>

void app_leds_init(void)
{
    hal_gpio_set(LED_PWR_PORT, LED_PWR_PIN, true);
    hal_gpio_set(LED_STS_PORT, LED_STS_PIN, false);
}

void app_leds_task(void)
{
    /* Power LED: solid on after boot (set once in app_leds_init, nothing
     * to do here — re-asserted anyway in case something else drove it). */
    hal_gpio_set(LED_PWR_PORT, LED_PWR_PIN, true);

    /* Status LED: unconditional ~2 Hz heartbeat = "scheduler is alive".
     * This is the primary no-debugger proof-of-life on the bench; the
     * task runs at DEFAULT_TASK_LED_MS (250 ms) precisely for this 2 Hz
     * rate. (Previously gated on g_system_state.usb_connected, so it went
     * dark on battery-only.) BLE / status-code patterns fold in here in WP5. */
    bool active = ((hal_systick_get_ms() / 250U) % 2U) != 0U;
    hal_gpio_set(LED_STS_PORT, LED_STS_PIN, active);
}
