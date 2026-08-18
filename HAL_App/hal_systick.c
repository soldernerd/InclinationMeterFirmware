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

uint32_t hal_systick_elapsed_ms(uint32_t start_ms)
{
    /* Unsigned subtraction wraps correctly even across a hal_systick_get_ms()
     * rollover, as long as the true elapsed time is under ~49.7 days. */
    return (uint32_t)(hal_systick_get_ms() - start_ms);
}

void hal_systick_delay_ms(uint32_t ms)
{
    HAL_Delay(ms);
}
