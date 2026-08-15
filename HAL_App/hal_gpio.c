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
     * defensive re-assertion, not the first time the rail comes up.) */
    LL_GPIO_ResetOutputPin(PWR_3V3_EN_PORT, PWR_3V3_EN_PIN);

    /* 5V rail: the display, buzzer, and both temp sensors need this.
     * No power-sequencing module exists yet — asserted unconditionally at
     * boot alongside 3V3 as a stopgap, not a real reference-counted design. */
    LL_GPIO_SetOutputPin(PWR_5V_EN_PORT, PWR_5V_EN_PIN);

    /* Let both rails settle before anything downstream (SPI, display) uses
     * them — comfortable margin over typical LDO start-up time, imperceptible
     * at boot. */
    HAL_Delay(20);

    /* Display CS idle LOW (Sharp LCD: CS active HIGH) */
    LL_GPIO_ResetOutputPin(DISP_CS_PORT, DISP_CS_PIN);

    /* Display starts off — drv_sharp_lcd_init() will turn it on */
    LL_GPIO_ResetOutputPin(DISP_ON_PORT, DISP_ON_PIN);

    /* LEDs off */
    LL_GPIO_ResetOutputPin(LED_PWR_PORT, LED_PWR_PIN);
    LL_GPIO_ResetOutputPin(LED_STS_PORT, LED_STS_PIN);

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
