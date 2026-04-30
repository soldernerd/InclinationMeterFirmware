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
