#include "math_displacement.h"
#include "math_phasor.h"
#include <math.h>

void math_phase_sincos(int16_t cdeg, float *cos_out, float *sin_out)
{
    float rad = (float)cdeg * (3.14159265f / 18000.0f);
    *cos_out = cosf(rad);
    *sin_out = sinf(rad);
}

void math_sensor_cal_make(MathSensorCal *out, int32_t k_micro, uint32_t pga, int32_t zero_ppm,
                          float phase_cos, float phase_sin)
{
    out->phase_cos  = phase_cos;
    out->phase_sin  = phase_sin;
    out->inv_pga    = 1.0f / (float)pga;
    out->inv_k      = 1.0e6f / (float)k_micro;
    out->zero_ratio = (float)zero_ppm * 1.0e-6f;
}

/* One sensor, given the shared reciprocal of D = A - B. */
static void sensor_delta(float iS, float qS, const MathSensorCal *cal,
                         float inv_den_re, float inv_den_im,
                         float *delta_out, float *residual_out)
{
    /* S / PGA -- k is stored for PGA = 1. */
    float num_re = iS * cal->inv_pga;
    float num_im = qS * cal->inv_pga;

    /* u = num * (1/D): complex multiply by the precomputed reciprocal. */
    float u_re = num_re * inv_den_re - num_im * inv_den_im;
    float u_im = num_re * inv_den_im + num_im * inv_den_re;

    /* Phase calibration: u lies along e^{j delta} for a real tilt, delta being
     * the sensor's delay relative to the run-time reference D. Rotate by -delta
     * so the tilt is exactly in phase and the quadrature part is a clean
     * diagnostic. */
    float x_re =  u_re * cal->phase_cos + u_im * cal->phase_sin;
    float x_im = -u_re * cal->phase_sin + u_im * cal->phase_cos;

    *delta_out    = (x_re - cal->zero_ratio) * cal->inv_k;
    *residual_out = x_im;
}

bool math_batch_demod(const MathBatchSums *s, const MathSensorCal cal[2], MathBatchResult *out)
{
    float iB = (float)s->iB, qB = (float)s->qB;
    float iA = (float)s->iA, qA = (float)s->qA;

    float inv_den_re, inv_den_im;
    if (!math_complex_reciprocal(iA - iB, qA - qB, &inv_den_re, &inv_den_im)) {
        return false;
    }
    sensor_delta((float)s->iS1, (float)s->qS1, &cal[0], inv_den_re, inv_den_im,
                 &out->delta[0], &out->residual[0]);
    sensor_delta((float)s->iS2, (float)s->qS2, &cal[1], inv_den_re, inv_den_im,
                 &out->delta[1], &out->residual[1]);
    return true;
}

bool math_phasor_exceeds(int64_t i_sum, int64_t q_sum, double max_mag)
{
    double i = (double)i_sum, q = (double)q_sum;
    return (i * i + q * q) > (max_mag * max_mag);
}

float math_zero_cal_new_zero(int32_t zero_ppm, int32_t k_micro, float avg_step1, float avg_step2)
{
    return (float)zero_ppm / (float)k_micro + (avg_step1 + avg_step2) / 2.0f;
}
