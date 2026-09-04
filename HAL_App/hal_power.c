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

/* Arbitrary 32-bit value, chosen only to be implausible as an
 * uninitialised/garbage TAMP_BKP0R content after a power-on reset (which
 * does clear the backup domain, unlike a software reset). */
#define DFU_REBOOT_MAGIC   0x44465521U   /* loosely "DFU!" */

static uint32_t s_last_bkp0r_seen = 0U;

uint32_t hal_power_last_bkp0r(void)
{
    return s_last_bkp0r_seen;
}

static void backup_domain_unlock(void)
{
    __HAL_RCC_PWR_CLK_ENABLE();
    HAL_PWR_EnableBkUpAccess();
    __HAL_RCC_RTCAPB_CLK_ENABLE();   /* TAMP_BKPxR needs the RTC APB clock */
}

void hal_power_request_dfu_reboot(void)
{
    backup_domain_unlock();
    TAMP->BKP0R = DFU_REBOOT_MAGIC;
    NVIC_SystemReset();
    for (;;) { }   /* NVIC_SystemReset() does not return */
}

void hal_power_check_dfu_request_and_jump(void)
{
    backup_domain_unlock();
    s_last_bkp0r_seen = TAMP->BKP0R;
    if (s_last_bkp0r_seen != DFU_REBOOT_MAGIC) {
        return;   /* normal boot — the common case */
    }
    TAMP->BKP0R = 0U;   /* one-shot: don't loop back into DFU on the next reset */

    /* Classic STM32 "jump to the ROM system bootloader" sequence: undo
     * whatever HAL_Init() just did (this runs immediately after it, before
     * any of our own clock/peripheral setup), remap system Flash to
     * address 0x0, then load the bootloader's own initial SP and reset
     * vector from there and jump. __HAL_SYSCFG_REMAPMEMORY_SYSTEMFLASH()
     * is the vendor-verified way to do the remap on this exact part —
     * deliberately not hand-computing a raw system-memory address, which
     * differs across STM32 families and would be easy to get wrong. */
    __disable_irq();
    SysTick->CTRL = 0U;
    HAL_RCC_DeInit();
    for (uint8_t i = 0U; i < 8U; i++) {
        NVIC->ICER[i] = 0xFFFFFFFFU;
        NVIC->ICPR[i] = 0xFFFFFFFFU;
    }

    __HAL_RCC_SYSCFG_CLK_ENABLE();
    __HAL_SYSCFG_REMAPMEMORY_SYSTEMFLASH();

    __enable_irq();

    typedef void (*BootJumpFn)(void);
    uint32_t   bootloader_sp   = *(volatile uint32_t *)0x00000000U;
    BootJumpFn bootloader_jump = (BootJumpFn)(*(volatile uint32_t *)0x00000004U);

    __set_MSP(bootloader_sp);
    bootloader_jump();

    for (;;) { }   /* never reached */
}
