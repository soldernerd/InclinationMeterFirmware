#include "hal_dfu.h"
#include "stm32g0xx_hal.h"

/* STM32G0x1 system memory (ROM bootloader) base — AN2606. 28 KB at this
 * address on every G0B1; word 0 is the bootloader's initial MSP, word 1
 * its reset vector. */
#define SYSMEM_BASE   0x1FFF0000UL

/* Retained-RAM sentinels (see linker script .noinit + hal_dfu.h). */
#define DFU_REQUEST_MAGIC   0xB00710ADUL   /* "reboot into the bootloader" */
#define DFU_BOUNCED_MAGIC   0xB00710BCUL   /* "the jump fell through"      */

/* .noinit: not cleared by the C startup, retained across NVIC_SystemReset,
 * lost on power-on / BOR / Standby exit. */
static volatile uint32_t s_dfu_request __attribute__((section(".noinit")));

static bool s_bounced_this_boot;

void hal_dfu_request_and_reset(void)
{
    s_dfu_request = DFU_REQUEST_MAGIC;
    __DSB();
    NVIC_SystemReset();
    for (;;) { }
}

bool hal_dfu_consume_bounce_flag(void)
{
    bool b = s_bounced_this_boot;
    s_bounced_this_boot = false;
    return b;
}

void hal_dfu_check_and_jump(void)
{
    uint32_t req = s_dfu_request;

    if (req == DFU_BOUNCED_MAGIC) {
        /* Previous boot jumped to the bootloader and control came back.
         * Record it for a normal-path log line; continue booting the app. */
        s_dfu_request = 0U;
        s_bounced_this_boot = true;
        return;
    }
    if (req != DFU_REQUEST_MAGIC) {
        return;
    }

    /* Consume the request up front. If the jump below falls through we
     * re-arm with DFU_BOUNCED_MAGIC and reset, so a wedged bootloader
     * can't trap us in the for(;;) with IRQs off — the device always
     * comes back to a running app on the next reset. */
    s_dfu_request = 0U;
    __DSB();

    /* Runs before HAL_Init(): clocks are at reset default, no peripheral
     * is enabled, no IRQ is active, SysTick is off. Belt-and-braces only. */
    __disable_irq();
    SysTick->CTRL = 0U;
    SysTick->LOAD = 0U;
    SysTick->VAL  = 0U;

    /* Map system memory to 0x00000000 so any low-address vector fetch the
     * bootloader makes before it sets its own VTOR lands in ROM. */
    RCC->APBENR2 |= RCC_APBENR2_SYSCFGEN;
    (void)RCC->APBENR2;
    __HAL_SYSCFG_REMAPMEMORY_SYSTEMFLASH();
    /* Barrier BEFORE the SP/PC reads — without it the remap write and the
     * two loads below have no ordering guarantee against each other. */
    __DSB();
    __ISB();

    SCB->VTOR = SYSMEM_BASE;

    const uint32_t *vt = (const uint32_t *)SYSMEM_BASE;
    uint32_t boot_sp = vt[0];
    uint32_t boot_pc = vt[1];

    __set_MSP(boot_sp);
    __DSB();
    __ISB();

    ((void (*)(void))boot_pc)();

    /* Fell through: the bootloader returned instead of taking over.
     * Re-arm the breadcrumb and reset to a clean app boot. */
    s_dfu_request = DFU_BOUNCED_MAGIC;
    __DSB();
    NVIC_SystemReset();
    for (;;) { }
}
