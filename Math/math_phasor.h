#ifndef MATH_PHASOR_H
#define MATH_PHASOR_H

#include <stdint.h>
#include <stdbool.h>

#define MATH_PHASOR_SAMPLES_PER_CYCLE 8U

/* Combines ONE channel's per-position batch sums into the batch's I/Q
 * phasor (2026-10-03, replaces per-sample weighted accumulation in the hot
 * path).
 *
 * pos_sum[n] is the plain sum of every sample that fell on position n
 * (0..7) of the 8-sample carrier cycle over one whole batch -- i.e. the
 * hot path just does `pos_sum[idx] += sample` (one 32-bit add per sample,
 * no multiply, no sign logic, no 64-bit math). The eight weights are the
 * Q14 8-point DFT-bin coefficients {0, +-1, +-0.707}:
 *   cos: {16384, 11585, 0, -11585, -16384, -11585, 0, 11585}
 *   sin: {0, 11585, 16384, 11585, 0, -11585, -16384, -11585}
 * so, by linearity and exactly (integer arithmetic), the weighted sum over
 * the batch is
 *   I = 16384*(s0 - s4) + 11585*(s1 - s3 - s5 + s7)
 *   Q = 16384*(s2 - s6) + 11585*(s1 + s3 - s5 - s7)
 * -- bit-for-bit what summing sample*weight per sample (the old
 * the per-sample reference in tests/test_math_phasor.c) gives, at ~1/30 of the cycles on a Cortex-M0+
 * (which has no 64-bit multiply: every int64 product was a ~50 cycle
 * library call, 8 per ADC sample).
 *
 * Range: each pos_sum[n] is a sum of batch_cycles signed 24-bit codes, so
 * |pos_sum| <= batch_cycles * 2^23 -- fits int32 for batch_cycles <= 256
 * (MATH_PHASOR_POS_SUM_MAX_CYCLES; Services/svc_displacement.c static-asserts
 * its batch size against it). The combination below is done in int64 (the
 * 4-term groups can reach 2^31 and the products ~2^45).
 *
 * Scale: |I + jQ| = 65536 * N * X_code for a peak amplitude X_code over N
 * cycles, same as before; like before it is NOT a physical unit and cancels
 * in the cross-channel ratio. */
#define MATH_PHASOR_POS_SUM_MAX_CYCLES 256U

void math_phasor_combine(const int32_t pos_sum[MATH_PHASOR_SAMPLES_PER_CYCLE],
                         int64_t *i_out, int64_t *q_out);

/* Complex reciprocal: (inv_re_out + j*inv_im_out) = 1 / (re + j*im).
 * Returns false (outputs left untouched) if re/im are exactly zero,
 * avoiding a division-by-zero fault.
 *
 * Callers that need to divide several numerators by the *same*
 * denominator (e.g. Services/svc_displacement.c's two sensors sharing
 * one (A-B) denominator) should call this once and multiply by the
 * result instead of dividing by the denominator each time -- this is
 * the one division a complex division needs, done once instead of once
 * per use. */
bool math_complex_reciprocal(float re, float im,
                              float *inv_re_out, float *inv_im_out);

#endif /* MATH_PHASOR_H */
