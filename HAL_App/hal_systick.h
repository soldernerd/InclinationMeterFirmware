#ifndef HAL_SYSTICK_H
#define HAL_SYSTICK_H

#include <stdint.h>

void     hal_systick_init(void);
uint32_t hal_systick_get_ms(void);

/* Busy-wait for at least `us` microseconds. Polls the SysTick VAL register
 * directly, so it does NOT depend on the SysTick interrupt and is safe to
 * call from any context, including an ISR (unlike HAL_Delay). */
void     hal_systick_delay_us(uint32_t us);

#endif /* HAL_SYSTICK_H */
