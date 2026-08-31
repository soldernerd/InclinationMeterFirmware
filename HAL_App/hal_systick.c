#include "hal_systick.h"
#include "stm32g0xx_hal.h"

void hal_systick_init(void)
{
    /* CubeMX-generated HAL_Init() already configures SysTick at 1 ms.
     * HAL_IncTick() runs in SysTick_Handler. Nothing extra to do. */
}

uint32_t hal_systick_get_ms(void)
{
    return HAL_GetTick();
}

void hal_systick_delay_us(uint32_t us)
{
    /* Accumulate elapsed SysTick counts (it counts down, reloads at
     * LOAD+1) until we've seen at least `us` worth. SysTick clocks at
     * SystemCoreClock (64 MHz => 64 counts/us). Works mid-ISR because it
     * never waits on the SysTick interrupt. Generous callers pass
     * milliseconds here on purpose. */
    const uint32_t reload  = SysTick->LOAD + 1U;
    uint32_t       target  = us * (SystemCoreClock / 1000000U);
    uint32_t       prev    = SysTick->VAL;
    uint32_t       elapsed = 0U;

    while (elapsed < target) {
        uint32_t now = SysTick->VAL;
        elapsed += (prev >= now) ? (prev - now) : (prev + reload - now);
        prev = now;
    }
}
