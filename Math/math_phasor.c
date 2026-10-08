#include "math_phasor.h"
#include <math.h>

/* Built with the -O2 pin CMakeLists.txt gives this file (since 2026-10-03 only
 * math_phasor_combine(), once per batch, is called by the firmware). Fail the build if the pin is lost. */
#if !defined(__OPTIMIZE__)
#error "hot-path file built without optimisation -- restore the -O2 pin in CMakeLists.txt"
#endif

/* The Q14 DFT-bin weights (cos/sin at n*45 degrees, x16384) are applied in
 * math_phasor_combine() below as {16384, 11585} products; the per-sample
 * reference implementation lives in tests/test_math_phasor.c. The ADS131M04
 * samples at exactly 8x the AD9833 excitation frequency by design
 * (Config/config.h's ADS131M04_OSR_FIELD derivation). */

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

static uint8_t s_n = MATH_PHASOR_SAMPLES_PER_CYCLE;
static int32_t s_cq[MATH_PHASOR_MAX_N];
static int32_t s_sq[MATH_PHASOR_MAX_N];

bool math_phasor_set_n(uint8_t n)
{
    if (n < 4U || n > MATH_PHASOR_MAX_N) {
        return false;
    }
    const float w = 6.2831853f / (float)n;
    for (uint8_t k = 0; k < n; ++k) {
        s_cq[k] = (int32_t)lroundf(16384.0f * cosf(w * (float)k));
        s_sq[k] = (int32_t)lroundf(16384.0f * sinf(w * (float)k));
    }
    s_n = n;
    return true;
}

uint8_t math_phasor_get_n(void)
{
    return s_n;
}

void math_phasor_combine_n(const int32_t *pos_sum, int64_t *i_out, int64_t *q_out)
{
    if (s_n == MATH_PHASOR_SAMPLES_PER_CYCLE) {
        math_phasor_combine(pos_sum, i_out, q_out);
        return;
    }
    int64_t ai = 0, aq = 0;
    for (uint8_t k = 0; k < s_n; ++k) {
        ai += (int64_t)pos_sum[k] * s_cq[k];
        aq += (int64_t)pos_sum[k] * s_sq[k];
    }
    *i_out = ai;
    *q_out = aq;
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
