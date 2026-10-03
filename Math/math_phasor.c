#include "math_phasor.h"

/* Built with the -O2 pin CMakeLists.txt gives this file (historically the
 * per-sample hot path; since 2026-10-03 only math_phasor_combine(), once per
 * batch, is called by the firmware). Fail the build if the pin is lost. */
#if !defined(__OPTIMIZE__)
#error "hot-path file built without optimisation -- restore the -O2 pin in CMakeLists.txt"
#endif

/* cos/sin at n*45 degrees (n=0..7), Q14-scaled (x16384) -- the trivial
 * DFT-bin coefficients for an 8-samples/cycle carrier (used by the
 * reference math_phasor_accumulate(); the firmware's hot path sums raw
 * samples per cycle position and math_phasor_combine() applies these
 * weights once per batch) (the ADS131M04
 * samples at exactly 8x the AD9833 excitation frequency by design --
 * Config/config.h's ADS131M04_OSR_FIELD derivation). Ported from the WP10
 * branch (wp10@eae360f, REV A) -- the math is unchanged, only the
 * consumer (Services/svc_displacement.c) is repointed at REV B's channel
 * mapping. */
static const int32_t s_cos_table[MATH_PHASOR_SAMPLES_PER_CYCLE] = {
    16384, 11585, 0, -11585, -16384, -11585, 0, 11585
};
static const int32_t s_sin_table[MATH_PHASOR_SAMPLES_PER_CYCLE] = {
    0, 11585, 16384, 11585, 0, -11585, -16384, -11585
};

void math_phasor_combine(const int32_t pos_sum[MATH_PHASOR_SAMPLES_PER_CYCLE],
                         int64_t *i_out, int64_t *q_out)
{
    /* All in int64: the 4-term groups can reach 2^31 (overflowing int32) and
     * the products ~2^45. 16384 is a power of two (the compiler emits a shift);
     * the 11585 products are the only real 64-bit multiplies -- 2 per
     * channel per batch. */
    const int64_t even_i = (int64_t)pos_sum[0] - pos_sum[4];
    const int64_t odd_i  = (int64_t)pos_sum[1] - pos_sum[3] - pos_sum[5] + pos_sum[7];
    const int64_t even_q = (int64_t)pos_sum[2] - pos_sum[6];
    const int64_t odd_q  = (int64_t)pos_sum[1] + pos_sum[3] - pos_sum[5] - pos_sum[7];
    *i_out = even_i * 16384 + odd_i * 11585;
    *q_out = even_q * 16384 + odd_q * 11585;
}

void math_phasor_accumulate(int32_t sample, uint8_t sample_idx,
                             int64_t *i_sum, int64_t *q_sum)
{
    if (sample_idx >= MATH_PHASOR_SAMPLES_PER_CYCLE) {
        return;
    }
    *i_sum += (int64_t)sample * s_cos_table[sample_idx];
    *q_sum += (int64_t)sample * s_sin_table[sample_idx];
}

bool math_complex_reciprocal(float re, float im,
                             float *inv_re_out, float *inv_im_out)
{
    float denom = re * re + im * im;
    if (denom == 0.0f) {
        return false;
    }
    float inv_denom = 1.0f / denom;
    *inv_re_out =  re * inv_denom;
    *inv_im_out = -im * inv_denom;
    return true;
}
