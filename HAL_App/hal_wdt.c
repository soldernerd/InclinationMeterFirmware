#include "hal_wdt.h"
#include "config.h"
#include "math_supervisor.h"
#include "stm32g0xx_hal.h"

/* See hal_wdt.h. Register-level (the CubeMX project does not enable the HAL WWDG module). */

#define WWDG_RELOAD  0x7FU   /* counter reload: 64 steps above the reset threshold of 0x3F */

static volatile uint32_t s_progress = 0;
static MathSupervisor    s_sup;
static volatile bool     s_running = false;

void hal_wdt_start(void)
{
#if WATCHDOG_ENABLED
    s_progress = 0;
    math_supervisor_init(&s_sup, s_progress);

    __HAL_RCC_WWDG_CLK_ENABLE();
    __HAL_DBGMCU_FREEZE_WWDG();              /* a halted debugger must not reset the target */

    /* prescaler /128 (WDGTB = 7), window = max so a refresh is always allowed, no early-wakeup interrupt */
    WWDG->CFR = (7UL << WWDG_CFR_WDGTB_Pos) | WWDG_CFR_W;
    WWDG->CR  = WWDG_CR_WDGA | WWDG_RELOAD;  /* activate: from here it cannot be stopped until the next reset */
    s_running = true;
#endif
}

void hal_wdt_progress(void)
{
    s_progress++;
}

void hal_wdt_tick_isr(void)
{
    if (!s_running) {
        return;
    }
    if (math_supervisor_tick(&s_sup, s_progress, WDT_STALL_LIMIT_MS)) {
        WWDG->CR = WWDG_CR_WDGA | WWDG_RELOAD;
    }
}

bool hal_wdt_running(void)
{
    return s_running;
}
