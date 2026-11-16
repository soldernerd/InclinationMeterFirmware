#ifndef MATH_NOISE_H
#define MATH_NOISE_H

/* Noise figures of the raw, unsmoothed batch readings over a sliding window (API Topics NOISE).
 * Pure logic -- no HAL, no globals -- host-tested in tests/test_math_noise.c.
 *
 * Why in the firmware: the host only sees the smoothed LIVE value (a Hann blend at ~4 Hz), so any noise it computes from
 * that stream understates the real noise and depends on the display bandwidth. The firmware has every batch (40.7/s).
 *
 * Five quantities are tracked per batch, all in the unit the caller feeds (mm/m for the API):
 *   0 S1, 1 S2, 2 Diff = S1 - S2 (sign-corrected tilt readings), 3 quadrature S1, 4 quadrature S2
 *   (Im(x) of the sensor ratio converted to the tilt scale -- independent of the tilt signal, so it separates sensor noise
 *   from real movement).
 * The window is the newest MATH_NOISE_LEN batches. Figures are `valid` only when the window is full; a gap in the batch
 * series (math_noise_break) or a reset empties it, so a figure never mixes data across a gap and a half-filled window is
 * never shown as final.
 *   sigma = sample standard deviation over the window (n - 1),
 *   p2p   = maximum minus minimum over the window.
 * The sums run in double on values centred on the first sample after a reset, so a large constant tilt does not cost
 * precision. */

#include <stdint.h>
#include <stdbool.h>

#define MATH_NOISE_LEN       407U   /* batches: 10 s at 40.7 batches/s */
#define MATH_NOISE_CHANNELS  5U

typedef struct {
    float    ring[MATH_NOISE_CHANNELS][MATH_NOISE_LEN];   /* centred values */
    double   sum[MATH_NOISE_CHANNELS];
    double   sumsq[MATH_NOISE_CHANNELS];
    float    ref[MATH_NOISE_CHANNELS];                    /* centring reference = the first sample after a reset */
    uint16_t next;                                        /* ring slot the next batch is written to */
    uint16_t count;                                       /* batches held, saturates at MATH_NOISE_LEN */
    bool     have_ref;
} MathNoise;

typedef struct {
    bool     valid;                                 /* the window is full */
    uint16_t count;                                 /* batches held (the figures are based on this many) */
    float    sigma[MATH_NOISE_CHANNELS];
    float    p2p[MATH_NOISE_CHANNELS];
} MathNoiseStats;

/* Empties the window (also used for "the series was interrupted"). */
void math_noise_reset(MathNoise *n);

/* Adds one batch: v[0..MATH_NOISE_CHANNELS-1]. */
void math_noise_push(MathNoise *n, const float v[MATH_NOISE_CHANNELS]);

/* Figures over what the window holds now. With fewer than 2 batches every sigma is 0; `valid` says whether to trust them. */
void math_noise_stats(const MathNoise *n, MathNoiseStats *out);

#endif /* MATH_NOISE_H */
