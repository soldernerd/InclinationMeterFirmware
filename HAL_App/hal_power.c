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

void hal_power_jump_to_system_bootloader(void)
{
    /* Classic STM32 "jump to the ROM system bootloader" sequence, called
     * directly from wherever the caller decides to (App/app_ui.c's
     * "Reboot to DFU" menu action) — no prior reset needed. Undoes
     * whatever this boot has set up (clocks, interrupts, SysTick), remaps
     * system Flash to address 0x0, then loads the bootloader's own
     * initial SP and reset vector from there and jumps.
     * __HAL_SYSCFG_REMAPMEMORY_SYSTEMFLASH() is the vendor-verified way to
     * do the remap on this exact part — deliberately not hand-computing a
     * raw system-memory address, which differs across STM32 families and
     * would be easy to get wrong.
     *
     * Interrupts: disabled up front so nothing fires mid-sequence while
     * clocks/NVIC are being torn down, but explicitly RE-enabled just
     * before the jump (after MSP is already pointed at the bootloader's
     * own stack, so anything that does fire lands safely). The bootloader
     * expects to be entered the way a real reset leaves the core — PRIMASK
     * clear, interrupts enabled — since that's the only way it's ever
     * normally reached; it has no reason to re-enable them itself. Leaving
     * interrupts globally disabled here means its own USB stack can never
     * see an interrupt, so it never notices a host talking to it, decides
     * nothing is happening, and falls back to jumping into the application
     * instead — exactly "no reboot but jumps back to normal operation".
     * Every NVIC line was cleared just above, so nothing stale is pending
     * to fire once this re-enables.
     *
     * USB_DRD_FS / CRS: confirmed by experiment that the ROM bootloader's
     * own USB works perfectly on this hardware (a blank chip's native
     * System-Memory boot enumerated and stayed resident for a full DFU
     * flash) — so a jump that still falls back to the app isn't a
     * hardware problem, it's this function leaving the bootloader a dirty
     * slate. HAL_RCC_DeInit() resets the clock TREE but doesn't force-
     * reset individual peripherals, so whatever CNTR/BCDR/CRS state our
     * own (not-yet-working) USB stack left behind was still sitting there
     * when the bootloader initialised its own USB on top of it — unlike a
     * genuine power-on reset, which starts every peripheral register at
     * its true default. Force-reset both explicitly so the bootloader
     * gets the same clean slate a real reset would have given it. Never
     * returns. */
    __disable_irq();
    SysTick->CTRL = 0U;
    HAL_RCC_DeInit();
    /* Cortex-M0+ (this core) has exactly ONE NVIC ICER/ICPR register —
     * CMSIS declares NVIC->ICER and NVIC->ICPR as [1], not [8] (the
     * bigger M3/M4/M7 layout most "jump to bootloader" examples online
     * are written against). A loop copied from one of those writes 7
     * words past the end of each 1-element array into reserved System
     * Control Space addresses — undefined behavior, and the actual
     * explanation for every one of the last several fixes (interrupt
     * re-enable, USB/CRS reset, VTOR) making no observable difference:
     * none of that code ever ran, because this was already corrupting
     * something (or faulting) before reaching it. */
    NVIC->ICER[0] = 0xFFFFFFFFU;
    NVIC->ICPR[0] = 0xFFFFFFFFU;

    __HAL_RCC_USB_FORCE_RESET();
    __HAL_RCC_CRS_FORCE_RESET();
    __HAL_RCC_USB_RELEASE_RESET();
    __HAL_RCC_CRS_RELEASE_RESET();

    __HAL_RCC_SYSCFG_CLK_ENABLE();
    __HAL_SYSCFG_REMAPMEMORY_SYSTEMFLASH();

    /* SCB->VTOR is a separate register from the SYSCFG remap above and
     * does NOT follow it automatically — it's an absolute address the
     * core uses for every exception/interrupt vector fetch from here on.
     * Our app never sets it explicitly (SystemInit()'s VTOR write is
     * conditional on USER_VECT_TAB_ADDRESS, which this project doesn't
     * define), so if it isn't already exactly 0x00000000, the very next
     * interrupt the bootloader takes (SysTick, USB, ...) vectors through
     * whatever table VTOR was ACTUALLY left pointing at — quite possibly
     * our own application's — rather than the bootloader's. Set it
     * explicitly rather than assume it was already correct. */
    SCB->VTOR = 0x00000000U;

    typedef void (*BootJumpFn)(void);
    uint32_t   bootloader_sp   = *(volatile uint32_t *)0x00000000U;
    BootJumpFn bootloader_jump = (BootJumpFn)(*(volatile uint32_t *)0x00000004U);

    __set_MSP(bootloader_sp);
    __DSB();
    __ISB();
    __enable_irq();
    bootloader_jump();

    for (;;) { }   /* never reached */
}
