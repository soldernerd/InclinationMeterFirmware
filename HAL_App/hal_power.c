#include "hal_power.h"
#include "stm32g0xx_hal.h"

void hal_power_configure_wakeup_pins(void)
{
    /* One call per pin — HAL_PWR_EnableWakeUpPin() only touches the bits
     * covered by the mask in its own argument, so these don't clobber
     * each other. All three are high-level detection (see pin_config.h's
     * ENC_1SW_PIN/ENC_2SW_PIN comments for why "active-low" mechanical
     * switches produce a HIGH signal at the MCU pin). */
    HAL_PWR_EnableWakeUpPin(PWR_WAKEUP_PIN1_HIGH);   /* ENC_1SW */
    HAL_PWR_EnableWakeUpPin(PWR_WAKEUP_PIN4_HIGH);   /* VBUS_SENSE */
    HAL_PWR_EnableWakeUpPin(PWR_WAKEUP_PIN5_HIGH);   /* ENC_2SW */
}

void hal_power_configure_rail_retention(void)
{
    /* PWR_GPIO_C/PWR_GPIO_BIT_6/7 must stay in sync with pin_config.h's
     * PWR_3V3_EN_PORT/PIN (GPIOC/GPIO_PIN_6) and PWR_5V_EN_PORT/PIN
     * (GPIOC/GPIO_PIN_7) — these are a separate PWR-peripheral macro
     * namespace (bit-position based, for PUCR/PDCR), not the same
     * constants as the GPIO HAL uses elsewhere in this codebase. */

    /* PC6 (!3V3_EN!): weak pull-UP holds the P-MOSFET gate high during
     * Standby -> MOSFET off -> 3.3V rail off. */
    (void)HAL_PWREx_EnableGPIOPullUp(PWR_GPIO_C, PWR_GPIO_BIT_6);

    /* PC7 (5V_EN): weak pull-DOWN holds the active-low shutdown input low
     * during Standby -> shutdown asserted -> 5V rail off. */
    (void)HAL_PWREx_EnableGPIOPullDown(PWR_GPIO_C, PWR_GPIO_BIT_7);

    /* Both pull configs above are inert until APC (Apply Pull
     * Configuration) is set — this is what actually makes them take
     * effect once Standby mode engages. */
    HAL_PWREx_EnablePullUpPullDownConfig();
}

bool hal_power_woke_from_standby(void)
{
    bool woke = __HAL_PWR_GET_FLAG(PWR_FLAG_SB) != 0U;
    __HAL_PWR_CLEAR_FLAG(PWR_FLAG_SB | PWR_FLAG_WUF);
    return woke;
}

void hal_power_enter_standby(void)
{
    /* Clear any wake flag latched by an earlier edge before sleeping.
     * The WKUP pins are enabled by hal_power_configure_wakeup_pins()
     * just before this call; if one of them (notably the encoder
     * switches on PA0/PC5, which idle at their active HIGH level) saw an
     * edge during the session, WUFx is already set. A pending WUFx makes
     * HAL_PWR_EnterSTANDBYMode() return immediately instead of sleeping,
     * leaving the MCU running with every rail powered down — a
     * dead-looking zombie state with no path back on, which is exactly
     * "powers off but never wakes on USB". Clearing here, after the pins
     * are enabled and immediately before entry, lets a fresh edge (USB
     * VBUS rising on WKUP4) be the thing that wakes it. */
    __HAL_PWR_CLEAR_FLAG(PWR_FLAG_WUF);

    HAL_PWR_EnterSTANDBYMode();

    /* Normally unreachable: Standby resets the MCU on wake. If control
     * DOES return here, Standby did not engage — the rails are already
     * down and there is no way back to a working state from this context,
     * so reboot. On the fresh boot, svc_battery re-derives state: if USB
     * is now present it charges instead of shutting down; if not, it
     * retries the shutdown. Beats silently running with the display dark. */
    NVIC_SystemReset();
}
