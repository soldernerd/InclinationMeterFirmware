#ifndef MATH_SUPERVISOR_H
#define MATH_SUPERVISOR_H

/* The decision behind the supervised watchdog (HAL_App/hal_wdt.c). The hardware watchdog (WWDG) has a timeout of only
 * about half a second, shorter than the longest legitimate pause of the cooperative main loop. So it is refreshed from
 * the 1 ms SysTick interrupt, but only while the main loop proves it is alive by advancing a progress counter:
 *   - the main loop hangs / livelocks      -> the counter stops -> after stall_limit_ms the refreshes stop -> reset;
 *   - the interrupts themselves are stuck   -> no refreshes at all -> the hardware resets within its own timeout.
 * Pure logic, host-tested: tests/test_math_supervisor.c. */

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    uint32_t last_progress;
    uint32_t stall_ms;
    bool     tripped;       /* latched: once the stall limit was reached the watchdog is never refreshed again */
} MathSupervisor;

void math_supervisor_init(MathSupervisor *s, uint32_t progress_now);

/* Call every millisecond with the main loop's progress counter. Returns true while the hardware watchdog may be
 * refreshed. */
bool math_supervisor_tick(MathSupervisor *s, uint32_t progress, uint32_t stall_limit_ms);

#endif /* MATH_SUPERVISOR_H */
