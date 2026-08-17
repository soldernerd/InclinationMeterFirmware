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
    if (pin < 16U) {
        s_exti_callbacks[pin] = cb;
    }
}

/* HAL_GPIO_EXTI_IRQHandler (called from CubeMX-generated EXTIn_m_IRQHandler
 * in stm32g0xx_it.c) dispatches to these weak callbacks based on which
 * edge fired. We register the same callback for both — the encoder driver
 * reads pin state inside its own callback to determine direction. */
static void exti_dispatch(uint16_t pin_mask)
{
    /* Single bit set per HAL invocation (GPIO_Pin is one-hot:
     * GPIO_PIN_0=0x0001 .. GPIO_PIN_15=0x8000); __builtin_ctz gives the
     * bit position directly instead of scanning for it. This runs in
     * interrupt context on every encoder edge, so an O(1) lookup here
     * matters more than in most of this codebase. pin_mask == 0 would be
     * undefined behavior for __builtin_ctz — defensively guarded even
     * though HAL never calls this with an empty mask. */
    if (pin_mask == 0U) {
        return;
    }
    uint8_t pin = (uint8_t)__builtin_ctz(pin_mask);
    if (s_exti_callbacks[pin]) {
        s_exti_callbacks[pin]();
    }
}

void HAL_GPIO_EXTI_Rising_Callback(uint16_t pin)  { exti_dispatch(pin); }
void HAL_GPIO_EXTI_Falling_Callback(uint16_t pin) { exti_dispatch(pin); }
