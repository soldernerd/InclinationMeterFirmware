#include "math_phasor.h"

/* cos/sin at n*45 degrees (n=0..7), Q14-scaled (x16384) -- the trivial
 * DFT-bin coefficients for an 8-samples/cycle carrier. Originally
 * inlined in Services/svc_signal_analysis.c (WP8); moved here so this
 * reusable numeric primitive isn't tied to one Services module -- see
 * Services/svc_displacement.c (WP10) for its current consumer. */
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

bool math_complex_div(float re1, float im1, float re2, float im2,
                       float *re_out, float *im_out)
{
    float denom = re2 * re2 + im2 * im2;
    if (denom == 0.0f) {
        return false;
    }
    *re_out = (re1 * re2 + im1 * im2) / denom;
    *im_out = (im1 * re2 - re1 * im2) / denom;
    return true;
}
