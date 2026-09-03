#include "hal_gpio.h"
#include "stm32g0xx_ll_gpio.h"
#include "pin_config.h"

static HalGpioExtiCallback s_exti_callbacks[16] = {0};

/* Pin mode/pull/speed are configured by MX_GPIO_Init() (CubeMX generated).
 * Initial output levels are set there too. We only enforce runtime defaults
 * here in case the MX init order changes in future.
 *
 * !3V3_EN! / !5V_EN! (the two switched power rails) are deliberately NOT
 * touched here — main() owns bringing them up, once each, at a well-defined
 * point in the boot sequence (after GPIO/LEDs/clocks, before anything that
 * needs them), not as a side effect of this function. See main.c. */
void hal_gpio_init(void)
{
    /* Display CS idle LOW (Sharp LCD: CS active HIGH) */
    LL_GPIO_ResetOutputPin(DISP_CS_PORT, DISP_CS_PIN);

    /* Display starts off — drv_sharp_lcd_init() will turn it on */
    LL_GPIO_ResetOutputPin(DISP_ON_PORT, DISP_ON_PIN);

    /* LEDs: deliberately NOT touched here. app_leds_init() now runs in
     * main() right after MX_GPIO_Init(), before this function, so LED_PWR
     * is already lit as the earliest possible proof of life — resetting
     * it here would just turn it back off until the scheduler starts.
     * LED state is app_leds.c's alone from here on (app_leds_init() /
     * app_leds_task()). */

    /* CHARGE_EN safe default: disabled (active-LOW, so HIGH = don't
     * charge). CubeMX's own reset-state default happens to leave this pin
     * LOW (charge enabled) — svc_battery_update() doesn't run and set the
     * real policy until its first scheduler tick (task_battery_ms, 1s
     * default), later still if EEPROM I/O is slow. Without this, every
     * boot — including a Standby wake, which is a full MCU reset — would
     * briefly enable charging regardless of battery state. */
    LL_GPIO_SetOutputPin(CHARGE_EN_PORT, CHARGE_EN_PIN);

    /* VBUS_SENSE (PA2), ENC_1SW (PA0), and ENC_2SW (PC5) are all configured
     * as SYS_WKUPx in CubeMX, which gets no MX_GPIO_Init() call — left in
     * POR-default Analog mode (input buffer disabled). The PWR peripheral's
     * wake-up detection (hal_power.c) samples these independently of GPIO
     * mode, but reconfiguring them as plain digital inputs here means
     * hal_gpio_get() also reads a real value in Run mode — doesn't conflict
     * with their WKUPx role. */
    LL_GPIO_SetPinMode(VBUS_SENSE_PORT, VBUS_SENSE_PIN, LL_GPIO_MODE_INPUT);
    LL_GPIO_SetPinPull(VBUS_SENSE_PORT, VBUS_SENSE_PIN, LL_GPIO_PULL_NO);
    LL_GPIO_SetPinMode(ENC_1SW_PORT, ENC_1SW_PIN, LL_GPIO_MODE_INPUT);
    LL_GPIO_SetPinPull(ENC_1SW_PORT, ENC_1SW_PIN, LL_GPIO_PULL_NO);
    LL_GPIO_SetPinMode(ENC_2SW_PORT, ENC_2SW_PIN, LL_GPIO_MODE_INPUT);
    LL_GPIO_SetPinPull(ENC_2SW_PORT, ENC_2SW_PIN, LL_GPIO_PULL_NO);
}

void hal_gpio_set(GPIO_TypeDef *port, uint16_t pin, bool state)
{
    if (state) {
        LL_GPIO_SetOutputPin(port, pin);
    } else {
        LL_GPIO_ResetOutputPin(port, pin);
    }
}

bool hal_gpio_get(GPIO_TypeDef *port, uint16_t pin)
{
    return LL_GPIO_IsInputPinSet(port, pin) ? true : false;
}

void hal_gpio_exti_register(uint8_t pin, HalGpioExtiCallback cb)
{
    /* WP1 stub — EXTI sources hooked up in WPx */
    if (pin < 16U) {
        s_exti_callbacks[pin] = cb;
    }
}
