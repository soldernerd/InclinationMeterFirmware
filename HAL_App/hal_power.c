#include "hal_power.h"
#include "stm32g0xx_hal.h"
#include "rtc.h"

/* RTC is a real CubeMX-managed peripheral now (Core/Src/rtc.c,
 * MX_RTC_Init() called unconditionally at boot from main.c, clocked from
 * LSE — Y1 is fitted on this board after all, CLAUDE.md's Open Item 2 was
 * stale). hal_power_reboot_to_dfu() below is the only consumer: it arms
 * the already-initialised hrtc's wakeup timer for exactly one purpose,
 * giving itself a self-triggered Standby wake-up (see that function's
 * comment for why). No separate init needed here — by the time any menu
 * action can run, MX_RTC_Init() has already executed. */

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

void hal_power_reboot_to_dfu(void)
{
    /* NOT a software jump into system memory — that approach (four rounds
     * of fixes: interrupts disabled, dirty USB/CRS state, SCB->VTOR not
     * following the SYSCFG remap, an NVIC array-bounds bug) never once
     * stayed resident in System Memory.
     *
     * v0.4.25/26 replaced it with FLASH_ACR.PROGEMPTY + NVIC_SystemReset():
     * force the "flash is blank" bit ST's own HAL_FLASHEx_ForceFlashEmpty()
     * exists for, reusing the exact mechanism a real mass-erase test had
     * already proven boots straight into System Memory and stays there for
     * a full DFU flash. That STILL landed back on the live screen — same
     * symptom as the software jump, despite being a completely different
     * mechanism. The reason: the boot-address decision (nBOOT0/nBOOT_SEL,
     * and this blank-chip override) is only RE-EVALUATED on a power-on
     * reset or on exiting Standby mode (RM0444 §2.5 — "the BOOT0 pin and
     * nBOOT1 bit are re-sampled when exiting from Standby mode"). A plain
     * NVIC_SystemReset() (AIRCR.SYSRESETREQ) is neither — on this newer
     * STM32 generation it's explicitly a "system reset" that leaves boot
     * configuration exactly as it was already latched at the last real
     * power-up, so forcing PROGEMPTY and then software-resetting can never
     * have worked, no matter how correct the FLASH_ACR write itself was.
     *
     * Fix: force PROGEMPTY (unchanged, still the right mechanism), then
     * get there via a Standby-mode bounce instead of a bare reset — this
     * project already uses real Standby entry for shutdown
     * (hal_power_enter_standby() above) and it demonstrably causes a full
     * reset-vector restart on wake, so it's a proven path to the kind of
     * reset that actually re-samples boot configuration. The one thing
     * Standby needs is a wake source, and the existing WKUP pins are all
     * wrong for this: VBUS_SENSE (WKUP4) is already asserted precisely
     * because a host is plugged in for the DFU flash, and a level-
     * triggered wake source that's already active at entry makes
     * HAL_PWR_EnterSTANDBYMode() abort into a dead-looking "running but
     * every rail is down" state instead of actually sleeping (see that
     * function's own comment) — never resetting at all. So this arms the
     * RTC wakeup timer instead: an internal source under firmware's own
     * control, immune to any pin already sitting at its wake level,
     * configured for the shortest possible period (raw RTCCLK/16, no
     * counts — a fraction of a millisecond on LSE) so the bounce is
     * effectively instant and needs no calendar/timekeeping setup.
     *
     * The application itself is left completely intact in flash the whole
     * time — nothing here erases or touches it. A genuine power-on reset
     * always re-evaluates real flash content and clears PROGEMPTY back to
     * "not empty" once real firmware exists again, so there's no way to
     * get permanently stuck: worst case, a power cycle instead of this
     * menu action gets back to the application immediately. Never
     * returns — falls through to the old reset-only behavior only if the
     * RTC/Standby setup itself fails, which is still strictly no worse
     * than v0.4.25/26. */
    HAL_FLASH_Unlock();
    HAL_FLASHEx_ForceFlashEmpty(FLASH_PROG_EMPTY);
    HAL_FLASH_Lock();

    if (HAL_RTCEx_SetWakeUpTimer_IT(&hrtc, 0, RTC_WAKEUPCLOCK_RTCCLK_DIV16) == HAL_OK) {
        HAL_PWR_DisableWakeUpPin(PWR_WAKEUP_PIN1 | PWR_WAKEUP_PIN4 | PWR_WAKEUP_PIN5);
        __HAL_PWR_CLEAR_FLAG(PWR_FLAG_WUF);
        HAL_PWR_EnterSTANDBYMode();
        /* Unreachable if Standby actually engaged. Falls through below. */
    }

    NVIC_SystemReset();
    for (;;) { }   /* NVIC_SystemReset() does not return */
}
