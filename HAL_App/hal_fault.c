#include "hal_fault.h"
#include "stm32g0xx_hal.h"

/* See hal_fault.h. The five TAMP backup registers hold MATH_FAULT_WORDS words (Math/math_fault.c). */

static MathFaultRecord s_last;

static volatile uint32_t *bkp(uint8_t i)
{
    return &((volatile uint32_t *)&TAMP->BKP0R)[i];
}

static void enable_access(void)
{
    __HAL_RCC_PWR_CLK_ENABLE();
    HAL_PWR_EnableBkUpAccess();
    __HAL_RCC_RTCAPB_CLK_ENABLE();
}

static void store(const MathFaultRecord *r)
{
    uint32_t w[MATH_FAULT_WORDS];
    math_fault_pack(r, w);
    enable_access();
    for (uint8_t i = 0; i < MATH_FAULT_WORDS; ++i) {
        *bkp(i) = w[i];
    }
}

void hal_fault_collect(void)
{
    enable_access();
    uint32_t w[MATH_FAULT_WORDS];
    for (uint8_t i = 0; i < MATH_FAULT_WORDS; ++i) {
        w[i] = *bkp(i);
    }
    if (math_fault_unpack(w, &s_last)) {
        for (uint8_t i = 0; i < MATH_FAULT_WORDS; ++i) {
            *bkp(i) = 0U;                      /* consumed: report it once */
        }
    }
}

const MathFaultRecord *hal_fault_last(void)
{
    return &s_last;
}

void hal_fault_record_init_failure(uint32_t caller_pc)
{
    MathFaultRecord r = { .kind = MATH_FAULT_INIT, .pc = caller_pc, .lr = 0, .xpsr = 0, .sp = 0 };
    store(&r);
}

void hal_fault_provoke(uint8_t kind)
{
    switch (kind) {
        case 1: {
            volatile uintptr_t addr = 1U;                      /* not a constant, so the compiler cannot see through it */
            volatile uint32_t *p = (volatile uint32_t *)addr;  /* an unaligned access: HardFault on the Cortex-M0+ */
            (void)*p;
            break;
        }
        case 2:
            for (;;) { }                                      /* the main loop hangs, interrupts keep running */
        case 3:
            __disable_irq();
            for (;;) { }                                      /* nothing runs, nothing refreshes the watchdog */
        default:
            break;
    }
}

void hal_fault_hardfault_c(uint32_t *frame)
{
    __disable_irq();
    MathFaultRecord r = { .kind = MATH_FAULT_HARDFAULT, .pc = 0, .lr = 0, .xpsr = 0, .sp = (uint32_t)(uintptr_t)frame };
    /* trust the frame only if it lies in SRAM (a fault caused by a broken stack pointer must not fault again) */
    if ((uintptr_t)frame >= 0x20000000UL && (uintptr_t)frame < 0x20000000UL + 0x24000UL - 32UL && ((uintptr_t)frame & 3U) == 0U) {
        r.lr   = frame[5];
        r.pc   = frame[6];
        r.xpsr = frame[7];
    }
    store(&r);
    NVIC_SystemReset();
    for (;;) { }
}
