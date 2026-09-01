#include "hal_gpio.h"
#include "stm32g0xx_ll_gpio.h"
#include "pin_config.h"

static HalGpioExtiCallback s_exti_callbacks[16] = {0};

/* Pin mode/pull/speed are configured by MX_GPIO_Init() (CubeMX generated).
 * Initial output levels are set there too. We only enforce runtime defaults
 * here in case the MX init order changes in future. */
void hal_gpio_init(void)
{
    /* 3V3 peripheral rail: OFF for WP1. Nothing in the WP1 firmware path
     * uses it (no EEPROM/sensors/BLE brought up), and the MCU runs from a
     * separate always-on supply, so gating it here is safe and drops
     * whatever leakage the unpowered peripherals were drawing. !3V3_EN! is
     * active-LOW, so drive HIGH = rail off (also CubeMX's boot default).
     * Re-enable (drive LOW) once a WP needs a 3V3 peripheral. */
    LL_GPIO_SetOutputPin(PWR_3V3_EN_PORT, PWR_3V3_EN_PIN);

    /* 5V rail: the display (and, later, buzzer/temp sensors) needs this.
     * No power-sequencing module exists yet — asserted unconditionally at
     * boot as a stopgap, not a real reference-counted design. */
    LL_GPIO_SetOutputPin(PWR_5V_EN_PORT, PWR_5V_EN_PIN);

    /* Let the 5V rail settle before anything downstream (SPI, display) uses
     * it — comfortable margin over typical regulator start-up time,
     * imperceptible at boot. */
    HAL_Delay(20);

    /* Display CS idle LOW (Sharp LCD: CS active HIGH) */
    LL_GPIO_ResetOutputPin(DISP_CS_PORT, DISP_CS_PIN);

    /* Display starts off — drv_sharp_lcd_init() will turn it on */
    LL_GPIO_ResetOutputPin(DISP_ON_PORT, DISP_ON_PIN);

    /* LEDs off */
    LL_GPIO_ResetOutputPin(LED_PWR_PORT, LED_PWR_PIN);
    LL_GPIO_ResetOutputPin(LED_STS_PORT, LED_STS_PIN);
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
