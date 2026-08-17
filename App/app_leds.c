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

    /* Status LED: 500 ms on / 500 ms off while USB is connected, off
     * otherwise. BLE patterns fold in here in WP5. */
    bool active = false;
    if (g_system_state.usb_connected) {
        active = ((hal_systick_get_ms() / 500U) % 2U) != 0U;
    }
    hal_gpio_set(LED_STS_PORT, LED_STS_PIN, active);
}
