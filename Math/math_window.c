#include "math_window.h"
#include <math.h>
#include <stddef.h>

/* Smallest floor allowed: keeps a pathological all-identical-residual stretch
 * (q == 0) from pinning the floor at 0, where the multiplicative creep could
 * never lift it. Real residual step power is ~1e-14 in the firmware's units; the
 * re-seed rule (math_window_reset()) recovers within minutes even from here. */
#define MATH_WINDOW_MIN_FLOOR  1.0e-20f

float math_hann_weights(float *w, uint16_t n)
{
    const float two_pi = 6.28318530717958647692f;
    for (uint16_t i = 0; i < n; ++i) {
        w[i] = 0.5f * (1.0f - cosf(two_pi * (float)(i + 1U) / (float)(n + 1U)));
    }
    return (float)(n + 1U) * 0.5f;
}

void math_window_reset(MathWindow *w, float creep_per_batch, uint32_t reseed_batches)
{
    for (uint8_t s = 0; s < 2U; ++s) {
        for (uint16_t i = 0; i < MATH_WINDOW_LEN; ++i) {
            w->delta[s][i] = 0.0f;
            w->q[s][i]     = 0.0f;
        }
        w->prev_r[s] = 0.0f;
        w->q_sum[s]  = 0.0;
        w->floor[s]  = MATH_WINDOW_MIN_FLOOR;
    }
    w->creep       = creep_per_batch;
    w->reseed_batches = reseed_batches;
    w->flag_run[0] = w->flag_run[1] = 0;
    w->floor_age   = 0;
    w->next        = 0;
    w->count       = 0;
    w->total       = 0;
    w->floor_valid = false;
}

void math_window_break(MathWindow *w)
{
    for (uint8_t s = 0; s < 2U; ++s) {
        for (uint16_t i = 0; i < MATH_WINDOW_LEN; ++i) {
            w->delta[s][i] = 0.0f;
            w->q[s][i]     = 0.0f;
        }
        w->prev_r[s] = 0.0f;
        w->q_sum[s]  = 0.0;
    }
    w->next  = 0;
    w->count = 0;
    w->total = 0;      /* so the first batch after the break again has no previous residual (q = 0) */
}

void math_window_push(MathWindow *w, const float reading[2], const float residual[2])
{
    const uint16_t i = w->next;
    const bool     full = (w->count == MATH_WINDOW_LEN);

    for (uint8_t s = 0; s < 2U; ++s) {
        float q = 0.0f;                         /* no previous residual for the very first batch */
        if (w->total > 0U) {
            const float step = residual[s] - w->prev_r[s];
            q = step * step;
        }
        if (full) {
            w->q_sum[s] -= (double)w->q[s][i];  /* the slot being overwritten leaves the window */
        }
        w->q[s][i]     = q;
        w->q_sum[s]   += (double)q;
        w->delta[s][i] = reading[s];
        w->prev_r[s]   = residual[s];
    }

    w->next = (uint16_t)((i + 1U) % MATH_WINDOW_LEN);
    if (!full) {
        w->count++;
    }
    w->total++;

    if (w->count == MATH_WINDOW_LEN) {
        const bool first = !w->floor_valid;
        for (uint8_t s = 0; s < 2U; ++s) {
            double sum = w->q_sum[s];
            if (sum < 0.0) {
                sum = 0.0;                      /* rounding of the running sum */
            }
            float m = (float)(sum / (double)MATH_WINDOW_LEN);
            if (first) {
                w->floor[s] = m;
            } else {
                w->floor[s] *= (1.0f + w->creep);   /* may rise only slowly ... */
                if (m < w->floor[s]) {
                    w->floor[s] = m;                /* ... and falls as soon as a quieter window appears */
                }
                /* ... except that a quiet level which has really changed must not leave
                 * every window flagged for hours (e.g. after a frozen channel recovers). */
                if (m > MATH_WINDOW_RESEED_RATIO * w->floor[s]) {
                    if (++w->flag_run[s] >= w->reseed_batches) {
                        w->floor[s]    = m;
                        w->flag_run[s] = 0;
                    }
                } else {
                    w->flag_run[s] = 0;
                }
            }
            if (w->floor[s] < MATH_WINDOW_MIN_FLOOR) {
                w->floor[s] = MATH_WINDOW_MIN_FLOOR;
            }
        }
        w->floor_valid = true;
        if (w->floor_age < UINT32_MAX) {
            w->floor_age++;
        }
    }
}

bool math_window_floor_ready(const MathWindow *w)
{
    /* the first full window seeds the floor (age 1); the second, independent
     * window is complete MATH_WINDOW_LEN batches later */
    return w->floor_valid && w->floor_age > MATH_WINDOW_LEN;
}

float math_window_mean_q(const MathWindow *w, uint8_t s, uint16_t n)
{
    if (n == 0U) {
        return 0.0f;
    }
    if (n >= MATH_WINDOW_LEN) {
        double sum = w->q_sum[s];
        return (float)((sum < 0.0 ? 0.0 : sum) / (double)MATH_WINDOW_LEN);
    }
    float sum = 0.0f;
    uint16_t idx = w->next;
    for (uint16_t j = 0; j < n; ++j) {
        idx = (uint16_t)((idx + MATH_WINDOW_LEN - 1U) % MATH_WINDOW_LEN);   /* newest first */
        sum += w->q[s][idx];
    }
    return sum / (float)n;
}

bool math_window_clean_sensor(const MathWindow *w, uint8_t s, uint16_t n, float k)
{
    if (!math_window_floor_ready(w)) {
        return true;
    }
    return math_window_mean_q(w, s, n) <= k * w->floor[s];
}

bool math_window_clean(const MathWindow *w, uint16_t n, float k)
{
    return math_window_clean_sensor(w, 0, n, k) && math_window_clean_sensor(w, 1, n, k);
}

float math_window_hann_mean(const MathWindow *w, uint8_t s, uint16_t n,
                            const float *weights, float wsum)
{
    float acc = 0.0f;
    uint16_t idx = (uint16_t)((w->next + MATH_WINDOW_LEN - n) % MATH_WINDOW_LEN);   /* oldest of the newest n */
    for (uint16_t i = 0; i < n; ++i) {
        acc += weights[i] * w->delta[s][idx];
        idx = (uint16_t)((idx + 1U) % MATH_WINDOW_LEN);
    }
    return acc / wsum;
}
