#include "hal_power.h"
#include "stm32g0xx_hal.h"

/* RTC (Core/Src/rtc.c, MX_RTC_Init(), clocked from LSE — Y1 is fitted on
 * this board after all, CLAUDE.md's Open Item 2 was stale) was added
 * specifically for a Standby-bounce DFU-reboot mechanism that turned out
 * to be structurally unworkable — see hal_power_reboot_to_dfu()'s comment.
 * Nothing in this file uses RTC any more, but it's left enabled in CubeMX
 * (harmless, and it did prove Standby entry/exit itself works correctly
 * via the LED diagnostic that mechanism briefly had) rather than churning
 * the .ioc again for something that may yet be useful. */

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
    /* Round 3 of this feature. History:
     *
     * v0.4.20-24: a direct software jump into System Memory (remap via
     * __HAL_SYSCFG_REMAPMEMORY_SYSTEMFLASH(), read the bootloader's own
     * SP/PC through the 0x0/0x4 alias, jump). Fixed four real bugs one at
     * a time (interrupts left disabled so the bootloader's own USB stack
     * never saw a host; dirty USB/CRS peripheral state left over from our
     * own not-yet-working stack; SCB->VTOR not following the SYSCFG remap
     * so the first interrupt vectored through the wrong table; an
     * NVIC->ICER/ICPR loop written for the 8-word M3/M4/M7 layout
     * silently corrupting memory past the M0+'s real 1-word arrays) —
     * and after all four, still landed straight back on the live screen.
     *
     * v0.4.25-30: switched to forcing FLASH_ACR.PROGEMPTY (the "flash is
     * blank" override ST's own HAL_FLASHEx_ForceFlashEmpty() exists for,
     * reusing the exact mechanism a real mass-erase test had already
     * proven works on this board) and reaching it via a genuine reset —
     * first a bare NVIC_SystemReset(), then, once that gave the same
     * result, a real Standby-mode entry/exit self-woken by the RTC
     * (confirmed by an LED flashing right before HAL_PWR_EnterSTANDBYMode
     * that Standby was genuinely reached). Both landed back on the live
     * screen too. The mechanical reason, worked out only after the
     * Standby attempt: FLASH_ACR is almost certainly in a domain Standby
     * itself powers down, so forcing PROGEMPTY and then triggering the
     * one kind of reset that actually re-samples it (RM0444 §2.5) is
     * self-defeating — Standby entry erases the forced bit before the
     * resulting reboot ever gets to read it. Every variant of this
     * approach needed a real reset to take effect, and every real reset
     * available here is exactly the kind of event that also wipes it.
     * Structurally unwinnable; abandoned.
     *
     * Back to the direct jump, this time with the one fix identified but
     * never actually tried: a missing memory barrier. SYSCFG's remap and
     * the two reads immediately after it (bootloader SP/PC through the
     * 0x0/0x4 alias) are both ordinary memory-mapped accesses with no
     * ordering guarantee between them on their own — without a barrier,
     * those reads can execute before the remap has actually taken effect,
     * silently returning this application's own vector table instead of
     * the bootloader's. v0.4.24 added __DSB()/__ISB() right before the
     * final jump (after the reads), which does nothing for this — the
     * barrier needs to sit BEFORE the reads, immediately after the remap
     * write, which no prior version had. */
    __disable_irq();
    SysTick->CTRL = 0U;
    HAL_RCC_DeInit();
    /* Cortex-M0+ (this core) has exactly ONE NVIC ICER/ICPR register —
     * CMSIS declares NVIC->ICER and NVIC->ICPR as [1], not [8]. */
    NVIC->ICER[0] = 0xFFFFFFFFU;
    NVIC->ICPR[0] = 0xFFFFFFFFU;

    __HAL_RCC_USB_FORCE_RESET();
    __HAL_RCC_CRS_FORCE_RESET();
    __HAL_RCC_USB_RELEASE_RESET();
    __HAL_RCC_CRS_RELEASE_RESET();

    __HAL_RCC_SYSCFG_CLK_ENABLE();
    __HAL_SYSCFG_REMAPMEMORY_SYSTEMFLASH();
    /* NEW: without this, nothing guarantees the remap above is actually
     * visible yet to the two reads below — they could complete against
     * the stale (pre-remap) memory map. */
    __DSB();
    __ISB();

    /* SCB->VTOR is a separate register from the SYSCFG remap and does not
     * follow it automatically — it's an absolute address used for every
     * exception/interrupt vector fetch from here on. Set explicitly
     * rather than assume it was already 0x00000000. */
    SCB->VTOR = 0x00000000U;

    typedef void (*BootJumpFn)(void);
    uint32_t   bootloader_sp   = *(volatile uint32_t *)0x00000000U;
    BootJumpFn bootloader_jump = (BootJumpFn)(*(volatile uint32_t *)0x00000004U);

    __set_MSP(bootloader_sp);
    __DSB();
    __ISB();
    __enable_irq();
    bootloader_jump();

    /* The jump has never actually stuck on this hardware (see the history
     * above): the ROM bootloader hands control straight back to the app
     * in main flash. If we reach here, fall back to a clean reset so the
     * device returns to a working state instead of hanging with RCC
     * de-init'd and interrupts re-enabled against a torn-down clock tree.
     * To reach DFU for real, power-cycle with BOOT0 asserted. */
    NVIC_SystemReset();
    for (;;) { }
}
