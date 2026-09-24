#include "math_phasor.h"

/* Hot path: MUST be built optimised. CMakeLists.txt pins this file to -O2
 * in every config -- math_phasor_accumulate() runs once per ADC sample
 * (Services/svc_displacement.c's on_sample(), inside the SysTick frame
 * drain alongside the ADS131M04 acquisition hot path it shares a build
 * pin with). Fail the build if the pin is lost. */
#if !defined(__OPTIMIZE__)
#error "hot-path file built without optimisation -- restore the -O2 pin in CMakeLists.txt"
#endif

/* cos/sin at n*45 degrees (n=0..7), Q14-scaled (x16384) -- the trivial
 * DFT-bin coefficients for an 8-samples/cycle carrier (the ADS131M04
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
