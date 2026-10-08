#ifndef MATH_FAULT_H
#define MATH_FAULT_H

/* The record a fault handler leaves for the next boot, as the five 32-bit words it fits into (the TAMP backup
 * registers survive a reset, see HAL_App/hal_fault.c). Pure packing with an integrity check, so a register that holds
 * power-on garbage is never mistaken for a fault. Host-tested: tests/test_math_fault.c. */

#include <stdint.h>
#include <stdbool.h>

#define MATH_FAULT_WORDS  5U

typedef enum {
    MATH_FAULT_NONE      = 0,
    MATH_FAULT_HARDFAULT = 1,   /* the CPU faulted: pc / lr / xpsr / sp are the stacked frame */
    MATH_FAULT_INIT      = 2,   /* Error_Handler(): an initialisation step failed; pc = the caller */
} MathFaultKind;

typedef struct {
    uint8_t  kind;              /* MathFaultKind */
    uint32_t pc, lr, xpsr, sp;
} MathFaultRecord;

/* words[0] = magic (16 bit) | check (8 bit) | kind (8 bit); words[1..4] = pc, lr, xpsr, sp. */
void math_fault_pack(const MathFaultRecord *r, uint32_t words[MATH_FAULT_WORDS]);

/* False (out zeroed) unless the magic and the check byte match and the kind is known. */
bool math_fault_unpack(const uint32_t words[MATH_FAULT_WORDS], MathFaultRecord *out);

#endif /* MATH_FAULT_H */
