#ifndef MATH_QUALITY_H
#define MATH_QUALITY_H

/* The LIVE display stream and the triggered precision measurement of
 * Services/svc_displacement.c as pure state machines on top of Math/math_window.h
 * -- no HAL, no globals, no clock (time arrives as an argument) -- so they are
 * host-tested (tests/test_math_quality.c) and replayed against recorded data
 * (tests/test_golden_displacement.c).
 *
 * Display stream: a Hann window over the newest `taps` contiguous batch readings,
 * published every `decimation`-th batch, with a per-sensor "doubtful" flag from the
 * window-level quality indicator (mean squared Im(x) step above k x the quiet
 * floor). The value is always published. A gap in the batch series (seq jump)
 * forgets the window, so no window spans a gap.
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
    float    hann_disp[MATH_WINDOW_LEN];
    float    hann_prec[MATH_WINDOW_LEN];
    float    hann_disp_sum, hann_prec_sum;
    uint16_t taps, decimation, prec_window;
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

/* taps, prec_window <= MATH_WINDOW_LEN, decimation >= 1; k, creep and
 * reseed_batches as for math_window_reset() / math_window_clean(). Computes the
 * Hann weights once; leaves the stream reset. */
void math_display_init(MathDisplay *d, uint16_t taps, uint16_t decimation, uint16_t prec_window,
                       float k, float creep, uint32_t reseed_batches);

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
