#include "hal_gpio.h"
#include "stm32g0xx_ll_gpio.h"
#include "pin_config.h"

static HalGpioExtiCallback s_exti_callbacks[16] = {0};

/* Pin mode/pull/speed are configured by MX_GPIO_Init() (CubeMX generated).
 * Initial output levels are set there too. We only enforce runtime defaults
 * here in case the MX init order changes in future. */
void hal_gpio_init(void)
{
    /* Keep the 3V3 rail on. !3V3_EN! is active-LOW — CubeMX's own GPIO init
     * defaults it HIGH (rail off) as a safe boot state; drive it LOW here.
     * (Also asserted pre-HAL_Init() in main.c via raw LL calls — this is a
     * defensive re-assertion, not the first time the rail comes up.)
     * WP1 briefly gated this off to chase current draw; WP2 needs it for the
     * EEPROM / TMP236 / battery ADC front-end, so it stays on from here. */
    LL_GPIO_ResetOutputPin(PWR_3V3_EN_PORT, PWR_3V3_EN_PIN);

    /* 5V rail: the display, buzzer, and both temp sensors need this.
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

    /* VBUS_SENSE (PA2) is configured as SYS_WKUP4 in CubeMX, which gets no
     * MX_GPIO_Init() call — the pin is left in POR-default Analog mode
     * (input buffer disabled) since Standby wake isn't wired up yet (no
     * HAL_PWR_EnableWakeUpPin() call exists). Reconfigure it as a plain
     * digital input here so hal_gpio_get() reads a real value in Run mode;
     * this doesn't conflict with its future use as a wake source. */
    LL_GPIO_SetPinMode(VBUS_SENSE_PORT, VBUS_SENSE_PIN, LL_GPIO_MODE_INPUT);
    LL_GPIO_SetPinPull(VBUS_SENSE_PORT, VBUS_SENSE_PIN, LL_GPIO_PULL_NO);
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
