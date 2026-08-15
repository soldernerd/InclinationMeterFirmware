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

bool hal_power_woke_from_standby(void)
{
    bool woke = __HAL_PWR_GET_FLAG(PWR_FLAG_SB) != 0U;
    __HAL_PWR_CLEAR_FLAG(PWR_FLAG_SB | PWR_FLAG_WUF);
    return woke;
}

void hal_power_enter_standby(void)
{
    HAL_PWR_EnterSTANDBYMode();
    /* Unreachable: Standby mode resets the MCU on wake rather than
     * returning here. */
}
