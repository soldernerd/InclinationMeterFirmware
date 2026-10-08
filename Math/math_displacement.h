#ifndef MATH_DISPLACEMENT_H
#define MATH_DISPLACEMENT_H

/* The per-batch demodulation arithmetic of Services/svc_displacement.c as pure
 * functions -- no HAL, no globals, no DeviceSettings -- so it is host-tested
 * (tests/test_math_displacement.c) and replayed against recorded data
 * (tests/test_golden_displacement.c).
 *
 * Model (docs/signal_processing.pdf, Sec. 9 and 13): per batch the ISR side has
 * produced, for each of the four ADC channels, the phasor I + jQ of the carrier
 * (math_phasor_combine). With A and B the two excitation channels and S a sensor
 * channel,
 *     D = A - B,   u = S / (PGA * D),   x = u * e^{-j phase},
 *     tilt [mm/m] = (Re x - zero) / k,   residual = Im x.
 * Re x is the in-phase part (the tilt), Im x the quadrature part (a diagnostic
 * that also drives the window quality indicator, Math/math_window.h). k is stored
 * for PGA = 1; PGA is the gain of the sensor's ADC channel. */

#include <stdint.h>
#include <stdbool.h>

/* One sensor's calibration, converted once from the EEPROM-backed scaled
 * integers (system_state.h) to the floats the per-batch arithmetic wants. */
typedef struct {
    float phase_cos, phase_sin;  /* cos/sin of the phase calibration */
    float inv_pga;               /* 1 / PGA of the sensor channel */
    float inv_k;                 /* 1 / k  (k per mm/m, at PGA 1) */
    float zero_ratio;            /* zero of the ratio r, dimensionless (level point) */
} MathSensorCal;

/* cos/sin of a phase calibration given in centidegrees. Split out because
 * sinf/cosf are soft-float library calls on the Cortex-M0+: the caller caches
 * the result and only recomputes it when the setting changes. */
void math_phase_sincos(int16_t cdeg, float *cos_out, float *sin_out);

/* k_micro: k x 1e6 (> 0, guarded by svc_storage_validate_settings()); pga >= 1;
 * zero_ppm: the zero in ppm of the ratio r; phase_cos/sin from math_phase_sincos(). */
void math_sensor_cal_make(MathSensorCal *out, int32_t k_micro, uint32_t pga, int32_t zero_ppm,
                          float phase_cos, float phase_sin);

/* One batch's phasors (the int64 sums math_phasor_combine() produced). */
typedef struct {
    int64_t iB, qB, iA, qA, iS1, qS1, iS2, qS2;
} MathBatchSums;

typedef struct {
    float delta[2];      /* tilt reading of S1, S2 (mm/m), native sign, before any invert flag */
    float residual[2];   /* Im x of S1, S2 */
} MathBatchResult;

/* Demodulates one batch. cal[0] = S1, cal[1] = S2. Returns false (out untouched)
 * if A - B is exactly zero (degenerate excitation) -- identical for both sensors,
 * so it is checked once, and the one reciprocal is shared (division is the most
 * expensive operation on an FPU-less core). */
bool math_batch_demod(const MathBatchSums *s, const MathSensorCal cal[2], MathBatchResult *out);

/* True if the phasor |I + jQ| exceeds max_mag, the largest value a full-scale,
 * undistorted carrier could produce over the batch. A data-integrity
 * assertion, NOT a clipping test. */
bool math_phasor_exceeds(int64_t i_sum, int64_t q_sum, double max_mag);

/* Flip (zero) calibration: the new absolute zero of one sensor, in mm/m, from the
 * stored zero (ppm of r, with its k) and the averaged readings of the two
 * orientations. The readings already have the old zero subtracted, so the
 * averaged residual bias is the correction still needed:
 *     new_zero = old_zero/k + (avg_step1 + avg_step2) / 2.
 * The 180 degree reversal flips a real tilt but not the instrument's zero error. */
float math_zero_cal_new_zero(int32_t zero_ppm, int32_t k_micro, float avg_step1, float avg_step2);

#endif /* MATH_DISPLACEMENT_H */
