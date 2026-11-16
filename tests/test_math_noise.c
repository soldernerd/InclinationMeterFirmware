/* Host tests for Math/math_noise.c -- sliding-window noise figures of the raw batch readings (API Topics NOISE). */
#include "test.h"
#include <math.h>
#include <string.h>

#include "../Math/math_noise.c"   /* single-TU test: pull the impl in directly */

static MathNoise s_n;

static uint32_t s_rng = 24681357u;
static float rnd_unit(void)      /* uniform in [-1, 1) */
{
    s_rng = s_rng * 1664525u + 1013904223u;
    return ((float)((s_rng >> 8) & 0xFFFFFFu) / 8388608.0f) - 1.0f;
}

static void push5(MathNoise *n, float a, float b, float c, float d, float e)
{
    float v[MATH_NOISE_CHANNELS] = { a, b, c, d, e };
    math_noise_push(n, v);
}

TEST(empty_window_is_not_valid_and_all_zero)
{
    math_noise_reset(&s_n);
    MathNoiseStats s;
    math_noise_stats(&s_n, &s);
    CHECK(!s.valid);
    CHECK_EQ(s.count, 0);
    for (int c = 0; c < (int)MATH_NOISE_CHANNELS; ++c) {
        CHECK(s.sigma[c] == 0.0f && s.p2p[c] == 0.0f);
    }
}

TEST(becomes_valid_exactly_when_the_window_is_full)
{
    math_noise_reset(&s_n);
    MathNoiseStats s;
    for (uint16_t i = 0; i < MATH_NOISE_LEN - 1U; ++i) {
        push5(&s_n, 1, 2, 3, 4, 5);
    }
    math_noise_stats(&s_n, &s);
    CHECK(!s.valid);
    CHECK_EQ(s.count, MATH_NOISE_LEN - 1U);
    push5(&s_n, 1, 2, 3, 4, 5);
    math_noise_stats(&s_n, &s);
    CHECK(s.valid);
    CHECK_EQ(s.count, MATH_NOISE_LEN);
}

TEST(a_constant_signal_has_zero_sigma_and_p2p_whatever_its_level)
{
    math_noise_reset(&s_n);
    for (uint16_t i = 0; i < MATH_NOISE_LEN + 50U; ++i) {
        push5(&s_n, 7.5f, -3.25f, 10.75f, 0.0f, 0.001f);
    }
    MathNoiseStats s;
    math_noise_stats(&s_n, &s);
    CHECK(s.valid);
    for (int c = 0; c < (int)MATH_NOISE_CHANNELS; ++c) {
        CHECK(s.sigma[c] < 1e-6f);
        CHECK(s.p2p[c] == 0.0f);
    }
}

TEST(sigma_and_p2p_of_a_known_pattern)
{
    /* alternating +-a: mean 0, sample sigma = a * sqrt(n / (n - 1)), p2p = 2a */
    math_noise_reset(&s_n);
    const float a = 0.004f;
    for (uint16_t i = 0; i < MATH_NOISE_LEN; ++i) {
        float v = (i & 1U) ? a : -a;
        push5(&s_n, v, 2.0f * v, 3.0f * v, 0.5f * v, 0.0f);
    }
    MathNoiseStats s;
    math_noise_stats(&s_n, &s);
    CHECK(s.valid);
    const float k = sqrtf((float)MATH_NOISE_LEN / (float)(MATH_NOISE_LEN - 1U));
    CHECK(fabsf(s.sigma[0] - a * k) < 1e-6f);
    CHECK(fabsf(s.sigma[1] - 2.0f * a * k) < 2e-6f);
    CHECK(fabsf(s.sigma[2] - 3.0f * a * k) < 3e-6f);
    CHECK(fabsf(s.sigma[3] - 0.5f * a * k) < 1e-6f);
    CHECK(s.sigma[4] < 1e-9f);
    CHECK(fabsf(s.p2p[0] - 2.0f * a) < 1e-6f);
    CHECK(fabsf(s.p2p[1] - 4.0f * a) < 2e-6f);
    CHECK(s.p2p[4] == 0.0f);
}

TEST(a_large_offset_does_not_cost_precision)
{
    /* 12 mm/m offset with 2 um/m of noise: float sums of squares would lose it, the centred double sums do not */
    math_noise_reset(&s_n);
    for (uint16_t i = 0; i < 3U * MATH_NOISE_LEN; ++i) {
        float v = 12.0f + 0.002f * rnd_unit();
        push5(&s_n, v, v, 0, 0, 0);
    }
    MathNoiseStats s;
    math_noise_stats(&s_n, &s);
    CHECK(s.valid);
    /* uniform +-0.002 -> sigma 0.002 / sqrt(3) = 0.001155; the input is rounded to float (about 1e-6 at 12) */
    CHECK(s.sigma[0] > 0.0009f && s.sigma[0] < 0.0014f);
}

TEST(the_window_slides_old_values_leave)
{
    math_noise_reset(&s_n);
    /* a big early outlier, then a full window of quiet: it must be gone */
    push5(&s_n, 100, 0, 0, 0, 0);
    for (uint16_t i = 0; i < MATH_NOISE_LEN - 1U; ++i) {
        push5(&s_n, 0, 0, 0, 0, 0);
    }
    MathNoiseStats s;
    math_noise_stats(&s_n, &s);
    CHECK(s.valid);
    CHECK(s.p2p[0] == 100.0f);                 /* still inside: the newest 407 include it */
    push5(&s_n, 0, 0, 0, 0, 0);                /* pushes it out */
    math_noise_stats(&s_n, &s);
    CHECK(s.valid);
    CHECK(s.p2p[0] == 0.0f);
    CHECK(s.sigma[0] < 1e-6f);
}

TEST(sliding_sums_match_a_direct_recomputation)
{
    math_noise_reset(&s_n);
    static float hist[2000];
    for (int i = 0; i < 2000; ++i) {
        hist[i] = 0.5f + 0.01f * rnd_unit() + 0.003f * sinf((float)i * 0.07f);
        push5(&s_n, hist[i], -hist[i], 0, 0, 0);
    }
    double m = 0, q = 0;
    for (int i = 2000 - (int)MATH_NOISE_LEN; i < 2000; ++i) { m += hist[i]; }
    m /= MATH_NOISE_LEN;
    for (int i = 2000 - (int)MATH_NOISE_LEN; i < 2000; ++i) { q += (hist[i] - m) * (hist[i] - m); }
    double want = sqrt(q / (MATH_NOISE_LEN - 1U));
    MathNoiseStats s;
    math_noise_stats(&s_n, &s);
    CHECK(fabs((double)s.sigma[0] - want) < 2e-6);
    CHECK(fabs((double)s.sigma[1] - want) < 2e-6);
}

TEST(reset_empties_the_window_and_the_reference)
{
    math_noise_reset(&s_n);
    for (uint16_t i = 0; i < MATH_NOISE_LEN; ++i) {
        push5(&s_n, 50.0f + (float)(i & 3U), 0, 0, 0, 0);
    }
    math_noise_reset(&s_n);
    MathNoiseStats s;
    math_noise_stats(&s_n, &s);
    CHECK(!s.valid && s.count == 0);
    for (uint16_t i = 0; i < MATH_NOISE_LEN; ++i) {
        push5(&s_n, -3.0f, 0, 0, 0, 0);
    }
    math_noise_stats(&s_n, &s);
    CHECK(s.valid);
    CHECK(s.p2p[0] == 0.0f);       /* nothing of the old window left */
}

int main(void)
{
    RUN(empty_window_is_not_valid_and_all_zero);
    RUN(becomes_valid_exactly_when_the_window_is_full);
    RUN(a_constant_signal_has_zero_sigma_and_p2p_whatever_its_level);
    RUN(sigma_and_p2p_of_a_known_pattern);
    RUN(a_large_offset_does_not_cost_precision);
    RUN(the_window_slides_old_values_leave);
    RUN(sliding_sums_match_a_direct_recomputation);
    RUN(reset_empties_the_window_and_the_reference);
    return test_summary();
}
