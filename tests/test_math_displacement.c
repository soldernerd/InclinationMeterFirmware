/* Host tests for Math/math_displacement.c -- the per-batch demodulation arithmetic. Synthetic phasors
 * with known tilt, phase and zero; the replay against recorded data is tests/test_golden_displacement.c. */
#include "test.h"
#include <math.h>
#include <string.h>

#include "../Math/math_phasor.c"
#include "../Math/math_displacement.c"

#define PI_F 3.14159265f

/* Builds the phasor sums of one batch for a given situation. The excitation A, B are fixed large
 * phasors; the sensor channel is S = PGA * D * r * e^{j phi} where D = A - B and r the in-phase ratio
 * that a tilt produces (tilt = (r - zero) / k). */
typedef struct { double a_re, a_im, b_re, b_im; } Excitation;

static void make_sums(MathBatchSums *s, const Excitation *e, double r1, double r2,
                      double phi1_deg, double phi2_deg, double pga1, double pga2)
{
    double d_re = e->a_re - e->b_re, d_im = e->a_im - e->b_im;
    double c1 = cos(phi1_deg * PI_F / 180.0), s1 = sin(phi1_deg * PI_F / 180.0);
    double c2 = cos(phi2_deg * PI_F / 180.0), s2 = sin(phi2_deg * PI_F / 180.0);
    /* S = pga * r * D * e^{j phi} */
    double u1re = r1 * (d_re * c1 - d_im * s1), u1im = r1 * (d_re * s1 + d_im * c1);
    double u2re = r2 * (d_re * c2 - d_im * s2), u2im = r2 * (d_re * s2 + d_im * c2);
    s->iA = (int64_t)e->a_re; s->qA = (int64_t)e->a_im;
    s->iB = (int64_t)e->b_re; s->qB = (int64_t)e->b_im;
    s->iS1 = (int64_t)(pga1 * u1re); s->qS1 = (int64_t)(pga1 * u1im);
    s->iS2 = (int64_t)(pga2 * u2re); s->qS2 = (int64_t)(pga2 * u2im);
}

static const Excitation EXC = { 3.0e12, 1.0e12, -2.0e12, 5.0e11 };   /* D = (5e12, 5e11) */

static void make_cals(MathSensorCal cal[2], int32_t k_micro, uint32_t pga, int32_t z1, int32_t z2,
                      int16_t ph1, int16_t ph2)
{
    float c, s;
    math_phase_sincos(ph1, &c, &s);
    math_sensor_cal_make(&cal[0], k_micro, pga, z1, c, s);
    math_phase_sincos(ph2, &c, &s);
    math_sensor_cal_make(&cal[1], k_micro, pga, z2, c, s);
}

static int near_rel(double got, double want, double rel, double abs_tol)
{
    return fabs(got - want) <= rel * fabs(want) + abs_tol;
}

TEST(phase_sincos_matches_libm_and_the_cardinal_angles)
{
    float c, s;
    math_phase_sincos(0, &c, &s);       CHECK(c == 1.0f && s == 0.0f);
    math_phase_sincos(9000, &c, &s);    CHECK(fabsf(c) < 1e-6f && fabsf(s - 1.0f) < 1e-6f);
    math_phase_sincos(-18000, &c, &s);  CHECK(fabsf(c + 1.0f) < 1e-6f && fabsf(s) < 1e-5f);
    math_phase_sincos(-600, &c, &s);    /* -6.00 degrees */
    CHECK(near_rel(c, cos(-6.0 * 3.14159265358979323846 / 180.0), 1e-6, 1e-7));
    CHECK(near_rel(s, sin(-6.0 * 3.14159265358979323846 / 180.0), 1e-6, 1e-7));
}

TEST(cal_make_converts_the_scaled_integers)
{
    MathSensorCal c;
    math_sensor_cal_make(&c, 21300, 16U, 1234, 0.5f, 0.25f);
    CHECK(near_rel(c.inv_pga, 1.0 / 16.0, 1e-7, 0));
    CHECK(near_rel(c.inv_k, 1.0e6 / 21300.0, 1e-6, 0));
    CHECK(near_rel(c.zero_ratio, 1234.0e-6, 1e-6, 0));
    CHECK(c.phase_cos == 0.5f && c.phase_sin == 0.25f);
}

TEST(a_known_tilt_comes_out_as_that_tilt_with_no_residual)
{
    /* tilt = (r - zero)/k with k = 0.0213 per mm/m (k_micro 21300), zero 0:  r = tilt*k */
    MathSensorCal cal[2];
    make_cals(cal, 21300, 1U, 0, 0, 0, 0);
    double tilt1 = 2.5, tilt2 = -1.25;
    MathBatchSums s;
    make_sums(&s, &EXC, tilt1 * 0.0213, tilt2 * 0.0213, 0.0, 0.0, 1.0, 1.0);
    MathBatchResult r;
    CHECK(math_batch_demod(&s, cal, &r));
    CHECK(near_rel(r.delta[0], tilt1, 1e-4, 1e-6));
    CHECK(near_rel(r.delta[1], tilt2, 1e-4, 1e-6));
    CHECK(fabsf(r.residual[0]) < 1e-6f && fabsf(r.residual[1]) < 1e-6f);
}

TEST(the_pga_is_divided_out_of_the_sensor_channel)
{
    MathSensorCal cal[2];
    make_cals(cal, 21300, 16U, 0, 0, 0, 0);
    MathBatchSums s;
    make_sums(&s, &EXC, 2.0 * 0.0213, 3.0 * 0.0213, 0.0, 0.0, 16.0, 16.0);   /* S is 16x larger */
    MathBatchResult r;
    CHECK(math_batch_demod(&s, cal, &r));
    CHECK(near_rel(r.delta[0], 2.0, 1e-4, 1e-6));
    CHECK(near_rel(r.delta[1], 3.0, 1e-4, 1e-6));
}

TEST(the_phase_calibration_puts_the_tilt_in_phase)
{
    /* the sensors lag the reference by -6 and -9.15 degrees; calibrated for exactly that, the tilt is
     * in phase (residual ~ 0); with the WRONG calibration part of it leaks into the residual */
    MathBatchSums s;
    make_sums(&s, &EXC, 1.0 * 0.0213, 1.0 * 0.0213, -6.0, -9.15, 1.0, 1.0);
    MathSensorCal good[2], none[2];
    make_cals(good, 21300, 1U, 0, 0, -600, -915);
    make_cals(none, 21300, 1U, 0, 0, 0, 0);
    MathBatchResult rg, rn;
    CHECK(math_batch_demod(&s, good, &rg));
    CHECK(math_batch_demod(&s, none, &rn));
    CHECK(near_rel(rg.delta[0], 1.0, 1e-3, 1e-5));
    CHECK(near_rel(rg.delta[1], 1.0, 1e-3, 1e-5));
    CHECK(fabsf(rg.residual[0]) < 1e-5f && fabsf(rg.residual[1]) < 1e-5f);
    CHECK(fabsf(rn.residual[0]) > 1e-3f);             /* sin(6 deg) * 0.0213 = 2.2e-3 */
    CHECK(rn.delta[0] < rg.delta[0]);                 /* and the in-phase part shrinks by cos(6 deg) */
}

TEST(the_zero_is_subtracted_in_the_ratio_domain)
{
    /* zero 1234 ppm of r: a ratio of exactly the zero reads 0 tilt, independent of k */
    MathBatchSums s;
    make_sums(&s, &EXC, 1234.0e-6, 1234.0e-6, 0.0, 0.0, 1.0, 1.0);
    MathSensorCal cal[2];
    make_cals(cal, 21300, 1U, 1234, 1234, 0, 0);
    MathBatchResult r;
    CHECK(math_batch_demod(&s, cal, &r));
    CHECK(fabsf(r.delta[0]) < 1e-4f && fabsf(r.delta[1]) < 1e-4f);
    make_cals(cal, 50000, 1U, 1234, 1234, 0, 0);       /* a different k does not move the level point */
    CHECK(math_batch_demod(&s, cal, &r));
    CHECK(fabsf(r.delta[0]) < 1e-4f && fabsf(r.delta[1]) < 1e-4f);
}

TEST(the_two_sensors_are_independent)
{
    MathSensorCal cal[2];
    make_cals(cal, 21300, 1U, 0, 0, 0, 0);
    MathBatchSums s;
    make_sums(&s, &EXC, 4.0 * 0.0213, 0.0, 0.0, 0.0, 1.0, 1.0);
    MathBatchResult r;
    CHECK(math_batch_demod(&s, cal, &r));
    CHECK(near_rel(r.delta[0], 4.0, 1e-4, 1e-6));
    CHECK(fabsf(r.delta[1]) < 1e-5f);
}

TEST(identical_excitation_phasors_are_degenerate_and_leave_the_result_untouched)
{
    MathSensorCal cal[2];
    make_cals(cal, 21300, 1U, 0, 0, 0, 0);
    MathBatchSums s;
    make_sums(&s, &EXC, 0.1, 0.1, 0.0, 0.0, 1.0, 1.0);
    s.iA = s.iB; s.qA = s.qB;                                   /* A == B */
    MathBatchResult r;
    r.delta[0] = 11.0f; r.delta[1] = 12.0f; r.residual[0] = 13.0f; r.residual[1] = 14.0f;
    CHECK(!math_batch_demod(&s, cal, &r));
    CHECK(r.delta[0] == 11.0f && r.delta[1] == 12.0f && r.residual[0] == 13.0f && r.residual[1] == 14.0f);
}

TEST(a_synthetic_batch_through_the_whole_chain_from_samples)
{
    /* end to end with math_phasor_combine: 8 position sums of a pure cosine give back its amplitude/phase */
    int32_t pos[8];
    for (int n = 0; n < 8; ++n) pos[n] = (int32_t)lrint(1.0e6 * cos(n * PI_F / 4.0 - 0.3));
    int64_t i, q;
    math_phasor_combine(pos, &i, &q);
    double mag = sqrt((double)i * (double)i + (double)q * (double)q);
    CHECK(near_rel(mag, 65536.0 * 1.0e6, 1e-3, 0));
    CHECK(near_rel(atan2((double)q, (double)i), 0.3, 1e-3, 1e-4));
}

TEST(exceeds_is_a_magnitude_test)
{
    CHECK(!math_phasor_exceeds(3, 4, 5.0));          /* exactly on the limit is not over it */
    CHECK(math_phasor_exceeds(3, 5, 5.0));
    CHECK(math_phasor_exceeds(-5, -1, 5.0));
    CHECK(!math_phasor_exceeds(0, 0, 0.0));
    CHECK(math_phasor_exceeds((int64_t)4.0e14, (int64_t)4.0e14, 5.0e14));
}

TEST(flip_calibration_isolates_the_zero_error)
{
    /* step 1 = phi + z, step 2 = -phi + z  =>  z = (step1 + step2) / 2, whatever the surface tilt phi */
    for (int t = -3; t <= 3; ++t) {
        float phi = 0.7f * (float)t, z = 0.31f;
        CHECK(near_rel(math_zero_cal_new_zero(0, 21300, phi + z, -phi + z), z, 1e-5, 1e-6));
    }
}

TEST(flip_calibration_adds_the_stored_zero_back)
{
    /* the readings already had the stored zero subtracted: the new zero = stored zero (in mm/m) + error */
    float stored_mm = 1234.0f / 21300.0f;     /* ppm / k_micro */
    float err = 0.05f;
    CHECK(near_rel(math_zero_cal_new_zero(1234, 21300, err, err), stored_mm + err, 1e-6, 1e-7));
    CHECK(near_rel(math_zero_cal_new_zero(-4321, 21300, 0.0f, 0.0f), -4321.0 / 21300.0, 1e-6, 1e-7));
}

int main(void)
{
    RUN(phase_sincos_matches_libm_and_the_cardinal_angles);
    RUN(cal_make_converts_the_scaled_integers);
    RUN(a_known_tilt_comes_out_as_that_tilt_with_no_residual);
    RUN(the_pga_is_divided_out_of_the_sensor_channel);
    RUN(the_phase_calibration_puts_the_tilt_in_phase);
    RUN(the_zero_is_subtracted_in_the_ratio_domain);
    RUN(the_two_sensors_are_independent);
    RUN(identical_excitation_phasors_are_degenerate_and_leave_the_result_untouched);
    RUN(a_synthetic_batch_through_the_whole_chain_from_samples);
    RUN(exceeds_is_a_magnitude_test);
    RUN(flip_calibration_isolates_the_zero_error);
    RUN(flip_calibration_adds_the_stored_zero_back);
    return test_summary();
}
