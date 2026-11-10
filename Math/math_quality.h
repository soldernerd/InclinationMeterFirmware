#ifndef MATH_QUALITY_H
#define MATH_QUALITY_H

/* The LIVE display stream and the triggered precision measurement of
 * Services/svc_displacement.c as pure state machines on top of Math/math_window.h
 * -- no HAL, no globals, no clock (time arrives as an argument) -- so they are
 * host-tested (tests/test_math_quality.c) and replayed against recorded data
 * (tests/test_golden_displacement.c).
 *
 * Display stream: published every `decimation`-th batch, with a per-sensor
 * "doubtful" flag from the window-level quality indicator over the newest
 * `quality_taps` batches (mean squared Im(x) step above k x the quiet floor). The
 * value is always published. A gap in the batch series (seq jump) forgets the
 * window, so no window spans a gap.
 *
 * The value is a variable-bandwidth blend of two Hann windows over the newest
 * contiguous batches: a FAST one (`fast_taps`, short latency) and a SLOW one
 * (`slow_taps`, quiet). While the instrument sits still the output is the slow
 * window; when the fast one departs from it by more than a threshold -- the tilt is
 * really moving -- the output follows the fast one, and hands back to the slow one
 * gradually as the slow one catches up (no jump back, no sliding back). The
 * threshold is max(blend_min, blend_k x sigma), sigma = the running rms of the
 * fast-slow difference with each sample clipped at the threshold (so a noisy site
 * raises it and a quiet one keeps it at blend_min). Until the slow window is full
 * (after a start or a gap) the output is the fast one.
 *
 * Precision measurement: after `prec_window` contiguous batches since the trigger,
 * every new batch forms a new sliding window; the first window that is clean for
 * both sensors is the result (its Hann-weighted means). A verdict needs the quiet
 * floor to be ready (math_window_floor_ready); until then the measurement waits.
 * The caller reports when its time budget is used up. */

#include <stdint.h>
#include <stdbool.h>
#include "math_window.h"

typedef struct {
    MathWindow win;
    float    hann_prec[MATH_WINDOW_LEN];
    float    hann_prec_sum;
    float    hann_fast[MATH_WINDOW_LEN];
    float    hann_slow[MATH_WINDOW_LEN];
    float    hann_fast_sum, hann_slow_sum;
    uint16_t fast_taps, slow_taps, quality_taps, decimation, prec_window;
    /* variable-bandwidth blend (math_display_set_blend) */
    float    blend_min, blend_k, blend_alpha, blend_hold;
    float    blend_sigma2[2];         /* running mean square of (fast - slow) while it is below the threshold */
    float    blend_w[2];              /* weight of the fast window in the output, 0..1 */
    float    k;                       /* quality threshold (power ratio to the floor) */
    float    creep;                   /* floor creep per batch (math_window_reset) */
    uint32_t reseed_batches;          /* floor re-seed time (math_window_reset) */
    uint8_t  phase;                   /* batches since the last published value */
    /* published display values */
    float    out[2];
    bool     doubtful[2];
    bool     valid;                   /* at least one value published since the last reset */
    uint16_t seq;                     /* bumps on every publish and every reset */
    /* contiguity */
    bool     have_seq;
    uint16_t last_seq;
} MathDisplay;

/* fast_taps <= slow_taps <= MATH_WINDOW_LEN, quality_taps and prec_window <=
 * MATH_WINDOW_LEN, decimation >= 1; k, creep and reseed_batches as for
 * math_window_reset() / math_window_clean(). Computes the Hann weights once;
 * leaves the stream reset. The blend parameters start at harmless defaults
 * (blend_min 0, k 6, alpha 1/240, hold 0.78); the caller sets its own with
 * math_display_set_blend(). */
void math_display_init(MathDisplay *d, uint16_t fast_taps, uint16_t slow_taps, uint16_t quality_taps,
                       uint16_t decimation, uint16_t prec_window,
                       float k, float creep, uint32_t reseed_batches);

/* blend_min: smallest threshold on |fast - slow| (the unit of the readings) that
 *   counts as "moving". blend_k: threshold = blend_k x sigma (6 = 6 standard
 *   deviations of the still-noise). sigma_alpha: per-publish weight of the noise
 *   estimate (1/240 = about 1 minute at 4 Hz). hold: per-publish decay of the fast
 *   weight after the movement stops (0.78 = about 1 s at 4 Hz). Resets the blend
 *   state (the next output is the fast window, fading to the slow one). */
void math_display_set_blend(MathDisplay *d, float blend_min, float blend_k, float sigma_alpha, float hold);

/* Forgets the history, the floors and the published values (a fresh start). seq
 * keeps counting so a consumer sees the change. */
void math_display_reset(MathDisplay *d);

/* A batch was skipped (degenerate excitation): no valid reading, and the next
 * batch must not join the current window. */
void math_display_skip(MathDisplay *d);

/* Feeds one batch. seq = the cycle counter of the batch's last cycle,
 * expected_step = cycles per batch: consecutive batches differ by exactly that
 * (modulo 65536). Returns false if this batch did NOT continue the series (the
 * first batch, a drop): the window was forgotten, and a precision measurement in
 * progress must be restarted too (math_precision_break). */
bool math_display_feed(MathDisplay *d, uint16_t seq, uint16_t expected_step,
                       const float delta[2], const float residual[2]);

typedef struct {
    uint16_t fill;        /* contiguous batches since the trigger (capped at the window length) */
    bool     disturbed;   /* the newest full window is not clean */
    bool     failed;      /* ended without a result */
    float    result[2];   /* Hann means of S1, S2 of the accepted window */
} MathPrecision;

typedef enum {
    MATH_PRECISION_WAITING = 0,   /* keep going */
    MATH_PRECISION_OK,            /* accepted: result[] valid */
    MATH_PRECISION_TIMEOUT        /* no clean window in time: failed, no value */
} MathPrecisionStep;

void math_precision_start(MathPrecision *p);   /* also used to cancel */
void math_precision_break(MathPrecision *p);   /* the series was interrupted: refill */

/* Call once per batch, after math_display_feed(). timed_out = the caller's time
 * budget is used up; it only takes effect if this batch did not produce a result. */
MathPrecisionStep math_precision_batch(MathPrecision *p, const MathDisplay *d, bool timed_out);

#endif /* MATH_QUALITY_H */
