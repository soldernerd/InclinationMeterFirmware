#ifndef HAL_WDT_H
#define HAL_WDT_H

#include <stdint.h>
#include <stdbool.h>

/* Supervised window watchdog (WWDG).
 *
 * Why the WWDG and not the IWDG: a started IWDG cannot be stopped, and depending on an option byte it keeps counting in
 * Standby, which would wake the instrument by itself and flatten the battery. The WWDG is clocked from the APB bus,
 * which stops in Standby, and is off again after every wake-up.
 *
 * The WWDG times out after only about half a second (4096 * 128 * 64 / 64 MHz = 524 ms), shorter than the longest
 * legitimate pause of the cooperative main loop. It is therefore refreshed from the 1 ms SysTick interrupt, and only
 * while the main loop proves it is alive (hal_wdt_progress() once per scheduler pass). Main loop stalled for
 * WDT_STALL_LIMIT_MS (config.h) -> refreshes stop -> reset within 524 ms. Interrupts stuck -> no refreshes -> reset
 * within 524 ms. The decision logic is Math/math_supervisor.c (host-tested).
 *
 * The debugger freezes it while halted. Start it late in main(), just before the scheduler: initialisation runs
 * unsupervised. */

void hal_wdt_start(void);          /* no effect when WATCHDOG_ENABLED is 0 */
void hal_wdt_progress(void);       /* main loop: call once per pass */
void hal_wdt_tick_isr(void);       /* SysTick handler, every 1 ms */
bool hal_wdt_running(void);

#endif /* HAL_WDT_H */
