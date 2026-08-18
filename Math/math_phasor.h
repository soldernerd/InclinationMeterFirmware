#ifndef MATH_PHASOR_H
#define MATH_PHASOR_H

#include <stdint.h>
#include <stdbool.h>

#define MATH_PHASOR_SAMPLES_PER_CYCLE 8U

/* Accumulates one sample's contribution to a running I/Q sum, using the
 * trivial 8-point DFT-bin coefficient set {0, +-1, +-0.707} (Q14-scaled,
 * so accumulation stays pure-integer) for a carrier sampled at exactly
 * MATH_PHASOR_SAMPLES_PER_CYCLE samples/cycle. sample_idx is the sample's
 * position within the current cycle (0..7) -- caller owns advancing and
 * wrapping it, and resetting *i_sum / *q_sum to 0 at the start of each
 * cycle. No-ops (leaves *i_sum / *q_sum untouched) if sample_idx is out of
 * range.
 *
 * The result is NOT normalized to a physical amplitude -- every channel
 * demodulated this way carries the same Q14/N-samples scale factor, so
 * for computations that take a RATIO across channels (e.g.
 * Services/svc_displacement.c's x = (S/k - B)/(A - B)) that factor
 * cancels and the raw sums can be used directly (just cast to float).
 * Only convert to true physical units if a caller needs an absolute
 * amplitude/phase on its own. */
void math_phasor_accumulate(int32_t sample, uint8_t sample_idx,
                             int64_t *i_sum, int64_t *q_sum);

/* Complex division: (re_out + j*im_out) = (re1 + j*im1) / (re2 + j*im2).
 * Returns false (re_out/im_out left untouched) if the denominator is
 * exactly zero, avoiding a division-by-zero fault. */
bool math_complex_div(float re1, float im1, float re2, float im2,
                       float *re_out, float *im_out);

#endif /* MATH_PHASOR_H */
