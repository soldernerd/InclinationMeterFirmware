/* Host tests for Math/math_window.c -- contiguous-batch history, Hann-weighted
 * means and the window-level quality indicator (LIVE display stream + the
 * triggered precision measurement). */
#include "test.h"
#include <math.h>
#include <string.h>

#include "../Math/math_window.c"   /* single-TU test: pull the impl in directly */

#define CREEP 1.42e-5f

static uint32_t s_rng = 987654321u;
static float rnd_unit(void)      /* uniform in [-1, 1) */
{
    s_rng = s_rng * 1664525u + 1013904223u;
    return ((float)((s_rng >> 8) & 0xFFFFFFu) / 8388608.0f) - 1.0f;
}

static void push_sample(MathWindow *w, float reading, float noise_amp)
{
    float rd[2] = { reading, reading };
    float rs[2] = { noise_amp * rnd_unit(), noise_amp * rnd_unit() };
    math_window_push(w, rd, rs);
}

TEST(hann_weights_sum_symmetry_and_shape)
{
    static float w[MATH_WINDOW_LEN];
    const uint16_t sizes[3] = { 25, 81, 10 };
    for (int k = 0; k < 3; ++k) {
        uint16_t n = sizes[k];
        float ws = math_hann_weights(w, n);
        float sum = 0.0f;
        for (uint16_t i = 0; i < n; ++i) {
            sum += w[i];
            CHECK(w[i] > 0.0f && w[i] <= 1.0f);
            CHECK(fabsf(w[i] - w[n - 1 - i]) < 1e-6f);          /* symmetric */
        }
        CHECK(fabsf(sum - ws) < 1e-3f);                          /* sums to (n+1)/2 */
        CHECK(fabsf(ws - (float)(n + 1) / 2.0f) < 1e-6f);
    }
    float w25[25];
    math_hann_weights(w25, 25);
    CHECK(w25[12] > 0.99f);                                      /* peak at the centre */
    CHECK(w25[0] < 0.02f && w25[24] < 0.02f);                    /* tiny at the ends */
}

TEST(hann_mean_of_a_ramp_is_its_centre_even_after_the_ring_wraps)
{
    static MathWindow w;
    static float w25[25], w81[81];
    float s25 = math_hann_weights(w25, 25);
    float s81 = math_hann_weights(w81, 81);
    math_window_reset(&w, CREEP);
    /* push 300 batches, reading = batch number (a linear ramp); the ring (81) wraps 3 times */
    for (int i = 0; i < 300; ++i) {
        push_sample(&w, (float)i, 0.0f);
    }
    /* newest value is 299; a symmetric window over 25 newest samples is centred on 299-12 */
    CHECK(fabsf(math_window_hann_mean(&w, 0, 25, w25, s25) - (299.0f - 12.0f)) < 1e-3f);
    CHECK(fabsf(math_window_hann_mean(&w, 1, 81, w81, s81) - (299.0f - 40.0f)) < 1e-3f);
}

TEST(a_constant_signal_averages_to_itself)
{
    static MathWindow w;
    static float w81[81];
    float s81 = math_hann_weights(w81, 81);
    math_window_reset(&w, CREEP);
    for (int i = 0; i < 200; ++i) push_sample(&w, 0.1234f, 1e-4f);
    CHECK(fabsf(math_window_hann_mean(&w, 0, 81, w81, s81) - 0.1234f) < 1e-6f);
}

TEST(window_is_reported_clean_until_a_floor_exists)
{
    static MathWindow w;
    math_window_reset(&w, CREEP);
    for (int i = 0; i < 40; ++i) push_sample(&w, 0.0f, 1e-4f);
    CHECK(!w.floor_valid);
    CHECK(math_window_clean(&w, 25, 6.0f));
    for (int i = 0; i < 100; ++i) push_sample(&w, 0.0f, 1e-4f);
    CHECK(w.floor_valid);
}

TEST(steady_noise_is_clean_a_burst_is_flagged_and_it_recovers)
{
    static MathWindow w;
    math_window_reset(&w, CREEP);
    for (int i = 0; i < 600; ++i) push_sample(&w, 0.0f, 1e-4f);          /* quiet history */
    CHECK(w.floor_valid);
    CHECK(math_window_clean(&w, 81, 6.0f));
    CHECK(math_window_clean(&w, 25, 6.0f));
    /* a transient: 30 batches with 20x larger residual noise (400x the step power) */
    for (int i = 0; i < 30; ++i) push_sample(&w, 0.0f, 2e-3f);
    CHECK(!math_window_clean(&w, 25, 6.0f));
    CHECK(!math_window_clean(&w, 81, 6.0f));
    /* quiet again: the 25-batch window clears first, the 81-batch window once the burst has left it */
    for (int i = 0; i < 26; ++i) push_sample(&w, 0.0f, 1e-4f);
    CHECK(math_window_clean(&w, 25, 6.0f));
    CHECK(!math_window_clean(&w, 81, 6.0f));
    for (int i = 0; i < 60; ++i) push_sample(&w, 0.0f, 1e-4f);
    CHECK(math_window_clean(&w, 81, 6.0f));
}

TEST(per_sensor_flags_are_independent)
{
    static MathWindow w;
    math_window_reset(&w, CREEP);
    for (int i = 0; i < 600; ++i) push_sample(&w, 0.0f, 1e-4f);
    for (int i = 0; i < 30; ++i) {                 /* disturb only sensor 1 (index 1) */
        float rd[2] = { 0.0f, 0.0f };
        float rs[2] = { 1e-4f * rnd_unit(), 2e-3f * rnd_unit() };
        math_window_push(&w, rd, rs);
    }
    CHECK(math_window_clean_sensor(&w, 0, 25, 6.0f));
    CHECK(!math_window_clean_sensor(&w, 1, 25, 6.0f));
    CHECK(!math_window_clean(&w, 25, 6.0f));
}

TEST(floor_creeps_up_slowly_under_a_persistent_disturbance_and_falls_at_once)
{
    static MathWindow w;
    math_window_reset(&w, CREEP);
    for (int i = 0; i < 600; ++i) push_sample(&w, 0.0f, 1e-4f);
    float quiet_floor = w.floor[0];
    /* 2 minutes (4900 batches) of a persistent 10x-amplitude disturbance */
    for (int i = 0; i < 4900; ++i) push_sample(&w, 0.0f, 1e-3f);
    CHECK(!math_window_clean(&w, 81, 6.0f));                 /* still flagged: the bar has not followed it */
    CHECK(w.floor[0] < 1.2f * quiet_floor);                  /* creep: +1.4e-5 per batch -> ~+7% in 2 min */
    /* back to quiet: the floor is the lowest window seen, so it stays at/below the old value */
    for (int i = 0; i < 200; ++i) push_sample(&w, 0.0f, 1e-4f);
    CHECK(math_window_clean(&w, 81, 6.0f));
    CHECK(w.floor[0] <= 1.2f * quiet_floor);
}

TEST(a_constant_residual_cannot_pin_the_floor_at_zero)
{
    static MathWindow w;
    math_window_reset(&w, CREEP);
    for (int i = 0; i < 400; ++i) push_sample(&w, 0.0f, 0.0f);   /* residual identically 0 */
    CHECK(w.floor_valid);
    CHECK(w.floor[0] >= 1.0e-30f);
    CHECK(math_window_clean(&w, 81, 6.0f));
    for (int i = 0; i < 100; ++i) push_sample(&w, 0.0f, 1e-4f);  /* noise appears: flagged, not NaN/inf */
    CHECK(!math_window_clean(&w, 81, 6.0f));
}

TEST(running_sum_matches_a_direct_recomputation)
{
    static MathWindow w;
    math_window_reset(&w, CREEP);
    for (int i = 0; i < 5000; ++i) push_sample(&w, 0.0f, 1e-4f * (1.0f + (i % 7)));
    double direct = 0.0;
    for (int i = 0; i < (int)MATH_WINDOW_LEN; ++i) direct += (double)w.q[0][i];
    CHECK(fabs(direct - w.q_sum[0]) < 1e-9 * direct + 1e-30);
    CHECK(fabsf(math_window_mean_q(&w, 0, MATH_WINDOW_LEN) - (float)(direct / MATH_WINDOW_LEN)) < 1e-6f * (float)(direct / MATH_WINDOW_LEN));
}

TEST(a_break_forgets_the_window_but_keeps_the_floor)
{
    static MathWindow w;
    static float w81[81];
    float s81 = math_hann_weights(w81, 81);
    math_window_reset(&w, CREEP);
    for (int i = 0; i < 300; ++i) push_sample(&w, 5.0f, 1e-4f);
    float fl = w.floor[0];
    math_window_break(&w);
    CHECK_EQ(w.count, 0);
    CHECK(w.floor_valid);
    CHECK(w.floor[0] == fl);
    for (int i = 0; i < 81; ++i) push_sample(&w, 1.0f, 1e-4f);
    CHECK_EQ(w.count, 81);
    /* no value from before the break leaks into the window */
    CHECK(fabsf(math_window_hann_mean(&w, 0, 81, w81, s81) - 1.0f) < 1e-6f);
    CHECK(math_window_clean(&w, 81, 6.0f));
}

int main(void)
{
    RUN(hann_weights_sum_symmetry_and_shape);
    RUN(hann_mean_of_a_ramp_is_its_centre_even_after_the_ring_wraps);
    RUN(a_constant_signal_averages_to_itself);
    RUN(window_is_reported_clean_until_a_floor_exists);
    RUN(steady_noise_is_clean_a_burst_is_flagged_and_it_recovers);
    RUN(per_sensor_flags_are_independent);
    RUN(floor_creeps_up_slowly_under_a_persistent_disturbance_and_falls_at_once);
    RUN(a_constant_residual_cannot_pin_the_floor_at_zero);
    RUN(running_sum_matches_a_direct_recomputation);
    RUN(a_break_forgets_the_window_but_keeps_the_floor);
    return test_summary();
}
