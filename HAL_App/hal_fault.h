#ifndef HAL_FAULT_H
#define HAL_FAULT_H

#include <stdint.h>
#include <stdbool.h>
#include "math_fault.h"

/* Fault capture across a reset.
 *
 * A HardFault used to spin forever, which left the display unrefreshed (the VCOM toggle runs from an interrupt that
 * cannot preempt the fault handler) and the instrument dead until a power cycle. Now the handler stores what it knows in
 * the TAMP backup registers (they survive a reset, not a loss of power) and resets. The next boot reads the record, logs
 * it and reports it in System HEALTH.
 *
 * The vector (Core/Src/stm32g0xx_it.c, USER CODE) is a naked stub that hands the stacked exception frame to
 * hal_fault_hardfault_c(). */

void hal_fault_collect(void);                       /* once, early in main(): reads and clears the record */
const MathFaultRecord *hal_fault_last(void);        /* kind == 0: the previous run did not end in a recorded fault */

/* Stores a record (kind, pc, lr) for the next boot; used by Error_Handler(). */
void hal_fault_record_init_failure(uint32_t caller_pc);

/* Test hook (API Commands FAULT_TEST): 1 = a HardFault, 2 = hang with interrupts running, 3 = hang with interrupts
 * disabled. Returns only for an unknown kind. */
void hal_fault_provoke(uint8_t kind);

/* HardFault entry, called with the stacked frame {r0, r1, r2, r3, r12, lr, pc, xpsr}. Does not return. */
void hal_fault_hardfault_c(uint32_t *frame) __attribute__((noreturn));

#endif /* HAL_FAULT_H */
