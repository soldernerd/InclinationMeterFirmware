#ifndef HAL_SYSTICK_H
#define HAL_SYSTICK_H

#include <stdint.h>

void     hal_systick_init(void);
uint32_t hal_systick_get_ms(void);

/* Wrap-safe "how many ms have elapsed since start_ms" — factors out a
 * pattern (unsigned-cast subtract against hal_systick_get_ms()) that was
 * independently hand-written at several call sites across the codebase.
 * Callers compare the result against their own threshold, e.g.
 * `if (hal_systick_elapsed_ms(start) >= timeout_ms) { ... }`. */
uint32_t hal_systick_elapsed_ms(uint32_t start_ms);

#endif /* HAL_SYSTICK_H */
