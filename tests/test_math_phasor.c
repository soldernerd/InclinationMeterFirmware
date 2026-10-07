/* Host tests for Math/math_phasor.c -- the per-position batch accumulation
 * (2026-10-03): the firmware's hot path adds each sample into a plain int32
 * sum for its position in the 8-sample cycle, and math_phasor_combine()
 * applies the Q14 DFT weights once per batch. This must give exactly the
 * integers the old per-sample weighted accumulation (math_phasor_accumulate() below) gives, and the int32 sums must never overflow. */
#include "test.h"
#include <string.h>

#include "../Math/math_phasor.c"   /* single-TU test: pull the impl in directly */

/* Reference weights and per-sample accumulate: the firmware's former hot path,
 * kept here as the oracle (cos/sin at n*45 degrees, Q14). */
static const int32_t s_cos_table[MATH_PHASOR_SAMPLES_PER_CYCLE] = {
    16384, 11585, 0, -11585, -16384, -11585, 0, 11585
};
static const int32_t s_sin_table[MATH_PHASOR_SAMPLES_PER_CYCLE] = {
    0, 11585, 16384, 11585, 0, -11585, -16384, -11585
};
static void math_phasor_accumulate(int32_t sample, uint8_t sample_idx, int64_t *i_sum, int64_t *q_sum)
{
    *i_sum += (int64_t)sample * s_cos_table[sample_idx];
    *q_sum += (int64_t)sample * s_sin_table[sample_idx];
}

#define CODE_MAX   8388607L    /* 2^23 - 1, ADS131M04 24-bit two's complement */
#define CODE_MIN  (-8388608L)

static uint32_t s_rng = 12345u;
static int32_t rnd_code(void)
{
    s_rng = s_rng * 1664525u + 1013904223u;          /* Numerical Recipes LCG */
    uint32_t r = (s_rng >> 8) & 0xFFFFFFu;            /* 24 random bits */
    return (int32_t)r - 8388608L;
}

/* Reference: old firmware behaviour, per-sample weighted 64-bit accumulate. */
static void ref_batch(const int32_t *x, unsigned n_samples, int64_t *i, int64_t *q)
{
    *i = 0; *q = 0;
    for (unsigned k = 0; k < n_samples; ++k) {
        math_phasor_accumulate(x[k], (uint8_t)(k % MATH_PHASOR_SAMPLES_PER_CYCLE), i, q);
    }
}

/* New firmware behaviour: int32 per-position sums + one combine. Returns the
 * largest |position sum| seen so the caller can check the int32 bound. */
static int64_t new_batch(const int32_t *x, unsigned n_samples, int64_t *i, int64_t *q)
{
    int32_t pos[MATH_PHASOR_SAMPLES_PER_CYCLE] = { 0 };
    for (unsigned k = 0; k < n_samples; ++k) {
        pos[k % MATH_PHASOR_SAMPLES_PER_CYCLE] += x[k];   /* the hot-path add */
    }
    math_phasor_combine(pos, i, q);
    int64_t m = 0;
    for (unsigned n = 0; n < MATH_PHASOR_SAMPLES_PER_CYCLE; ++n) {
        int64_t a = pos[n] < 0 ? -(int64_t)pos[n] : pos[n];
        if (a > m) m = a;
    }
    return m;
}

#define BATCH_CYCLES 64u
#define BATCH_SAMPLES (BATCH_CYCLES * MATH_PHASOR_SAMPLES_PER_CYCLE)

TEST(random_batches_match_the_per_sample_reference_exactly)
{
    static int32_t x[BATCH_SAMPLES];
    for (int t = 0; t < 500; ++t) {
        for (unsigned k = 0; k < BATCH_SAMPLES; ++k) x[k] = rnd_code();
        int64_t ri, rq, ni, nq;
        ref_batch(x, BATCH_SAMPLES, &ri, &rq);
        new_batch(x, BATCH_SAMPLES, &ni, &nq);
        CHECK_EQ(ni, ri);
        CHECK_EQ(nq, rq);
    }
}

TEST(full_scale_extremes_match_and_stay_inside_int32)
{
    static int32_t x[BATCH_SAMPLES];
    const int32_t levels[2] = { CODE_MAX, CODE_MIN };
    for (int a = 0; a < 2; ++a) {
        /* every sample at one rail */
        for (unsigned k = 0; k < BATCH_SAMPLES; ++k) x[k] = levels[a];
        int64_t ri, rq, ni, nq;
        ref_batch(x, BATCH_SAMPLES, &ri, &rq);
        int64_t m = new_batch(x, BATCH_SAMPLES, &ni, &nq);
        CHECK_EQ(ni, ri);
        CHECK_EQ(nq, rq);
        CHECK(m <= 2147483647LL);
        /* worst case for the 4-term groups: odd positions alternate rails */
        for (unsigned k = 0; k < BATCH_SAMPLES; ++k) {
            unsigned n = k % MATH_PHASOR_SAMPLES_PER_CYCLE;
            x[k] = (n == 1 || n == 7) ? levels[a] : (n == 3 || n == 5) ? levels[1 - a] : 0;
        }
        ref_batch(x, BATCH_SAMPLES, &ri, &rq);
        m = new_batch(x, BATCH_SAMPLES, &ni, &nq);
        CHECK_EQ(ni, ri);
        CHECK_EQ(nq, rq);
        CHECK(m <= 2147483647LL);
    }
}

TEST(position_sums_fit_int32_up_to_the_documented_cycle_limit)
{
    /* MATH_PHASOR_POS_SUM_MAX_CYCLES x the most negative code is exactly -2^31. */
    CHECK_EQ((long long)MATH_PHASOR_POS_SUM_MAX_CYCLES * CODE_MIN, -2147483648LL);
    CHECK((long long)MATH_PHASOR_POS_SUM_MAX_CYCLES * CODE_MAX < 2147483648LL);
    /* ...and the firmware's batch (64) leaves 4x headroom. */
    CHECK((long long)BATCH_CYCLES * CODE_MAX * 4 < 2147483648LL);
}

TEST(dc_is_rejected_exactly)
{
    static int32_t x[BATCH_SAMPLES];
    for (unsigned k = 0; k < BATCH_SAMPLES; ++k) x[k] = 1234567;
    int64_t i, q;
    new_batch(x, BATCH_SAMPLES, &i, &q);
    CHECK_EQ(i, 0);
    CHECK_EQ(q, 0);
}

TEST(a_pure_cosine_lands_in_I_with_the_documented_scale)
{
    /* x[n] = A*cos(n*45deg) (exact integers: A, A*0.707 rounded is not exact, so
     * use A=0 positions only where cos is 0/+-1): positions 0 and 4 carry +-A. */
    static int32_t x[BATCH_SAMPLES];
    const int32_t A = 1000000;
    for (unsigned k = 0; k < BATCH_SAMPLES; ++k) {
        unsigned n = k % MATH_PHASOR_SAMPLES_PER_CYCLE;
        x[k] = (n == 0) ? A : (n == 4) ? -A : 0;
    }
    int64_t i, q;
    new_batch(x, BATCH_SAMPLES, &i, &q);
    /* I = 16384*(s0 - s4) = 16384 * 2*A*BATCH_CYCLES */
    CHECK_EQ(i, (int64_t)16384 * 2 * A * BATCH_CYCLES);
    CHECK_EQ(q, 0);
}

int main(void)
{
    RUN(random_batches_match_the_per_sample_reference_exactly);
    RUN(full_scale_extremes_match_and_stay_inside_int32);
    RUN(position_sums_fit_int32_up_to_the_documented_cycle_limit);
    RUN(dc_is_rejected_exactly);
    RUN(a_pure_cosine_lands_in_I_with_the_documented_scale);
    return test_summary();
}
