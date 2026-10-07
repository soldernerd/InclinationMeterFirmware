#ifndef MATH_WINDOW_H
#define MATH_WINDOW_H

/* Contiguous-batch history, Hann-windowed means and the window-level quality
 * indicator used by the LIVE display stream and the triggered precision
 * measurement (Services/svc_displacement.c). Pure logic -- no HAL, no globals --
 * so it is host-tested (tests/test_math_window.c).
 *
 * Why this exists (Testing/2026-09-30_bulk_adc_30s_interval/findings.md): the
 * sensor readings contain a strong ~20 Hz resonance that is nearly coherent
 * between the two sensors. A smooth (Hann) window over contiguous 40.7 Hz batch
 * values removes it; dropping individual batches does NOT (it breaks the
 * cancellation). What can be detected is a disturbed WINDOW: the quadrature
 * part Im(x) of the sensor phasor moves in steps, and the mean squared
 * batch-to-batch step of Im(x) over a window, compared with the instrument's own
 * quiet floor, flags windows with transients (footsteps, bumps, a machine
 * starting). On a 19 h gapless recording, gating 2 s windows with this indicator
 * cut the daytime repeatability scatter 3x (0.078 -> 0.025 um nominal) at ~1%
 * failures and left the quiet-night result unchanged.
 *
 * Per batch (one call to math_window_push) the caller supplies, per sensor, the
 * reading delta and the residual Im(x). The module keeps the last
 * MATH_WINDOW_LEN batches, the squared residual steps q = (r_k - r_{k-1})^2,
 * and per sensor a floor tracker. */

#include <stdint.h>
#include <stdbool.h>

/* Longest window supported (batches) = the precision window, 81 batches =
 * 1.99 s at 40.7 Hz. Shorter windows (the display's 25) use the newest n. */
#define MATH_WINDOW_LEN 81U

/* Weights of a symmetric Hann window of n taps WITHOUT its zero end points,
 * w[i] = 0.5 * (1 - cos(2*pi*(i+1)/(n+1))), i = 0..n-1; their sum is
 * exactly (n+1)/2 (returned). */
float math_hann_weights(float *w, uint16_t n);

typedef struct {
    float  delta[2][MATH_WINDOW_LEN];   /* ring of the last batches' readings, per sensor */
    float  q[2][MATH_WINDOW_LEN];       /* ring of squared residual steps */
    float  prev_r[2];                   /* residual of the previous batch */
    double q_sum[2];                    /* sum of q over the ring contents (double: exact enough, no drift) */
    float  floor[2];                    /* quiet floor of the full-window mean q (min-follower with creep) */
    float  creep;                       /* fractional upward creep of the floor per batch */
    uint16_t next;                      /* ring index the next batch is written to */
    uint16_t count;                     /* batches held, saturates at MATH_WINDOW_LEN */
    uint32_t total;                     /* batches pushed since reset (never saturates before ~3 years) */
    bool   floor_valid;                 /* true once a full window has been seen */
} MathWindow;

/* creep_per_batch: how fast the floor may rise when every window is noisier
 * than it (a doubling every ~20 minutes = ln2/(1200 s * 40.7 Hz) = 1.4e-5). */
void math_window_reset(MathWindow *w, float creep_per_batch);

/* The batch series is no longer contiguous (a batch was dropped, or the
 * caller skipped one): forget the window contents so no window ever spans the
 * gap. The quiet floors are kept -- they describe the instrument, not the data. */
void math_window_break(MathWindow *w);

/* Adds one batch. reading[i] / residual[i] for sensor i = 0 (S1), 1 (S2).
 * Updates the squared-step rings and, once a full window is held, the floors. */
void math_window_push(MathWindow *w, const float reading[2], const float residual[2]);

/* Mean squared residual step of sensor s over the newest n batches
 * (n <= count). n == MATH_WINDOW_LEN uses the running sum. */
float math_window_mean_q(const MathWindow *w, uint8_t s, uint16_t n);

/* True when the newest n batches are clean for BOTH sensors: mean squared
 * residual step <= k * (the sensor's quiet floor). Until the floor is known (no
 * full window held yet) there is nothing to judge against, so the window is
 * reported clean. Needs n <= count. */
bool math_window_clean(const MathWindow *w, uint16_t n, float k);

/* Same, per sensor (the display flags each reading separately). */
bool math_window_clean_sensor(const MathWindow *w, uint8_t s, uint16_t n, float k);

/* Hann-weighted mean of the newest n readings of sensor s. weights = the n
 * weights from math_hann_weights(), wsum their sum. Needs n <= count. */
float math_window_hann_mean(const MathWindow *w, uint8_t s, uint16_t n,
                            const float *weights, float wsum);

#endif /* MATH_WINDOW_H */
