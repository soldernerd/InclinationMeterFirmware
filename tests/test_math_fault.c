/* Host tests for Math/math_fault.c -- the fault record that survives a reset in the TAMP backup registers. */
#include "test.h"
#include <string.h>

#include "../Math/math_fault.c"

static MathFaultRecord sample(uint8_t kind)
{
    MathFaultRecord r = { kind, 0x0800ABCDu, 0x08001235u, 0x61000000u, 0x20001F00u };
    return r;
}

TEST(a_record_round_trips)
{
    for (uint8_t kind = 1; kind <= 2; ++kind) {
        MathFaultRecord in = sample(kind), out;
        uint32_t w[MATH_FAULT_WORDS];
        math_fault_pack(&in, w);
        CHECK(math_fault_unpack(w, &out));
        CHECK_EQ(out.kind, kind);
        CHECK_EQ(out.pc, in.pc);
        CHECK_EQ(out.lr, in.lr);
        CHECK_EQ(out.xpsr, in.xpsr);
        CHECK_EQ(out.sp, in.sp);
    }
}

TEST(power_on_garbage_is_not_a_fault)
{
    MathFaultRecord out;
    uint32_t zeros[MATH_FAULT_WORDS] = {0};
    CHECK(!math_fault_unpack(zeros, &out));
    CHECK_EQ(out.kind, 0);
    uint32_t ones[MATH_FAULT_WORDS] = { 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu };
    CHECK(!math_fault_unpack(ones, &out));
    uint32_t pattern[MATH_FAULT_WORDS] = { 0xDEADBEEFu, 1, 2, 3, 4 };
    CHECK(!math_fault_unpack(pattern, &out));
    /* a cleared record (what hal_fault_collect() leaves behind) reads as nothing */
    MathFaultRecord in = sample(1);
    uint32_t w[MATH_FAULT_WORDS];
    math_fault_pack(&in, w);
    memset(w, 0, sizeof w);
    CHECK(!math_fault_unpack(w, &out));
}

TEST(any_single_corrupted_word_or_bit_is_detected)
{
    MathFaultRecord in = sample(1), out;
    uint32_t good[MATH_FAULT_WORDS];
    math_fault_pack(&in, good);
    int detected = 0, total = 0;
    for (unsigned word = 1; word < MATH_FAULT_WORDS; ++word) {
        for (unsigned bit = 0; bit < 32; ++bit) {
            uint32_t w[MATH_FAULT_WORDS];
            memcpy(w, good, sizeof w);
            w[word] ^= 1UL << bit;
            total++;
            if (!math_fault_unpack(w, &out)) detected++;
        }
    }
    CHECK_EQ(detected, total);                       /* a one-bit error in the payload always trips the check byte */
}

TEST(an_unknown_kind_is_rejected_even_with_a_valid_check)
{
    MathFaultRecord in = sample(7), out;
    uint32_t w[MATH_FAULT_WORDS];
    math_fault_pack(&in, w);
    CHECK(!math_fault_unpack(w, &out));
}

int main(void)
{
    RUN(a_record_round_trips);
    RUN(power_on_garbage_is_not_a_fault);
    RUN(any_single_corrupted_word_or_bit_is_detected);
    RUN(an_unknown_kind_is_rejected_even_with_a_valid_check);
    return test_summary();
}
