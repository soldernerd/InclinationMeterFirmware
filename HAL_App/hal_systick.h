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

/* Blocking millisecond delay -- thin wrapper so Drivers_App/ one-time init
 * sequences (e.g. drv_ads131m04.c's SYNC/RESET pulse) don't need to
 * #include the ST HAL directly (CLAUDE.md 8.1: upper layers go through
 * HAL_App/). Not for use in any periodic/scheduler-task code path. */
void hal_systick_delay_ms(uint32_t ms);

#endif /* HAL_SYSTICK_H */
