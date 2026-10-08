#include "math_fault.h"

#define MAGIC 0xFA17U

static uint8_t check_byte(uint8_t kind, const uint32_t w[MATH_FAULT_WORDS])
{
    uint32_t x = 0xA5U ^ kind;
    for (uint8_t i = 1; i < MATH_FAULT_WORDS; ++i) {
        x ^= w[i] ^ (w[i] >> 8) ^ (w[i] >> 16) ^ (w[i] >> 24);
    }
    return (uint8_t)x;
}

void math_fault_pack(const MathFaultRecord *r, uint32_t words[MATH_FAULT_WORDS])
{
    words[1] = r->pc;
    words[2] = r->lr;
    words[3] = r->xpsr;
    words[4] = r->sp;
    words[0] = ((uint32_t)MAGIC << 16) | ((uint32_t)check_byte(r->kind, words) << 8) | r->kind;
}

bool math_fault_unpack(const uint32_t words[MATH_FAULT_WORDS], MathFaultRecord *out)
{
    out->kind = 0;
    out->pc = out->lr = out->xpsr = out->sp = 0;
    if ((words[0] >> 16) != MAGIC) {
        return false;
    }
    uint8_t kind = (uint8_t)(words[0] & 0xFFU);
    if (kind != MATH_FAULT_HARDFAULT && kind != MATH_FAULT_INIT) {
        return false;
    }
    if ((uint8_t)((words[0] >> 8) & 0xFFU) != check_byte(kind, words)) {
        return false;
    }
    out->kind = kind;
    out->pc   = words[1];
    out->lr   = words[2];
    out->xpsr = words[3];
    out->sp   = words[4];
    return true;
}
