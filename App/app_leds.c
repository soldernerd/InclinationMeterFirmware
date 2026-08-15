#include "app_leds.h"
#include "hal_gpio.h"
#include "pin_config.h"
#include <stdbool.h>

static bool s_status_on = false;

void app_leds_init(void)
{
    hal_gpio_set(LED_PWR_PORT, LED_PWR_PIN, true);
    hal_gpio_set(LED_STS_PORT, LED_STS_PIN, false);
    s_status_on = false;
}

void app_leds_task(void)
{
    s_status_on = !s_status_on;
    hal_gpio_set(LED_STS_PORT, LED_STS_PIN, s_status_on);
}
