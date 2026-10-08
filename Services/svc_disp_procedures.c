#include "svc_displacement.h"
#include "svc_displacement_internal.h"
#include "math_displacement.h"
#include "math_quality.h"
#include "hal_systick.h"
#include "config.h"
#include "system_state.h"

/* The two triggered procedures on top of the running measurement, split out of svc_displacement.c:
 * the flip (zero) calibration and the precision measurement. Both are fed one batch at a time by
 * the service (disp_procs_batch) and are mutually exclusive. Task context only. */

/* --- Zero calibration state (2026-09-25) --- see svc_displacement.h's
 * comment. Task context only, same reasoning as the phasor log state
 * above (the "producer" is process_one_batch(), the "consumer" is
 * svc_api.c's svc_api_update(), both task-context callers). */
static DisplacementZeroCalPhase s_zero_cal_phase = DISP_ZERO_CAL_IDLE;
static uint16_t s_zero_cal_count = 0;
static uint8_t  s_zero_cal_mask  = ZERO_CAL_SENSORS_BOTH;   /* sensors covered by the current run */
static float    s_zero_cal_sum1  = 0.0f;   /* running sum for the CURRENT step */
static float    s_zero_cal_sum2  = 0.0f;
static float    s_zero_cal_step1_avg1 = 0.0f;   /* saved once step 1 completes */
static float    s_zero_cal_step1_avg2 = 0.0f;
static float    s_zero_cal_result1_mm = 0.0f;   /* new absolute zero_offset, once RESULT_READY */
static float    s_zero_cal_result2_mm = 0.0f;

/* --- Triggered precision measurement state (2026-09-26, redesigned
 * 2026-10-07) --- see svc_displacement.h's comment. Task context only, same
 * reasoning as the zero-cal state above (the "producer" is
 * process_one_batch(), the "consumer" is Services/svc_api.c's command
 * handler). */
static DisplacementPrecisionPhase s_precision_phase     = DISP_PRECISION_IDLE;
static MathPrecision s_prec;                     /* fill / disturbed / failed / result (Math/math_quality.h) */
static uint32_t s_precision_start_ms   = 0;

/* Feeds one batch's delta1_mm/delta2_mm into an in-progress zero-cal
 * step, if one is armed -- called unconditionally from process_one_batch()
 * (cheap no-op via the phase check when idle). Completing
 * DISPLACEMENT_ZERO_CAL_SAMPLES samples finishes the current step: step 1
 * just saves its average and waits for step2_begin(); step 2 combines
 * both steps' averages into the new absolute zero offset (config.h's
 * "Displacement zero calibration" comment has the derivation) and moves
 * to RESULT_READY for svc_api.c to consume. */
static void zero_cal_accumulate(float delta1, float delta2)
{
    if (s_zero_cal_phase != DISP_ZERO_CAL_STEP1_RUNNING
        && s_zero_cal_phase != DISP_ZERO_CAL_STEP2_RUNNING) {
        return;
    }
    s_zero_cal_sum1 += delta1;
    s_zero_cal_sum2 += delta2;
    if (++s_zero_cal_count < DISPLACEMENT_ZERO_CAL_SAMPLES) {
        return;
    }
    float avg1 = s_zero_cal_sum1 / (float)DISPLACEMENT_ZERO_CAL_SAMPLES;
    float avg2 = s_zero_cal_sum2 / (float)DISPLACEMENT_ZERO_CAL_SAMPLES;
    if (s_zero_cal_phase == DISP_ZERO_CAL_STEP1_RUNNING) {
        s_zero_cal_step1_avg1 = avg1;
        s_zero_cal_step1_avg2 = avg2;
        s_zero_cal_phase = DISP_ZERO_CAL_STEP1_DONE;
    } else {
        /* zero_error = (step1_avg + step2_avg) / 2 -- see config.h. The
         * NEW absolute offset is the OLD one plus that error: delta_mm
         * already has the old zero_offset subtracted (compute_sensor_delta()),
         * so the averaged residual bias IS the correction still needed.
         * disp_s1/s2_zero_offset_um is stored in the cal_mult-INDEPENDENT
         * theoretical domain (load_sensor_cal()'s 2026-09-29 comment), but
         * avg1/avg2 above are full compute_sensor_delta() outputs -- the
         * OUTPUT-mm domain, cal_mult already applied. Scale the stored
         * value up by the CURRENT cal_mult here so this combination happens
         * in one consistent domain; svc_api.c's zero_cal_apply_if_ready()
         * scales the finished result back down before persisting it, so
         * disp_s1/s2_zero_offset_um itself never leaves the theoretical
         * domain on disk. */
        /* zero_ppm / k_micro = the stored zero expressed in mm/m (both are
         * x1e-6); the averaged delta already has it subtracted. */
        s_zero_cal_result1_mm = math_zero_cal_new_zero(g_device_settings.disp_s1_zero_ppm,
                                                       g_device_settings.disp_s1_k_micro,
                                                       s_zero_cal_step1_avg1, avg1);
        s_zero_cal_result2_mm = math_zero_cal_new_zero(g_device_settings.disp_s2_zero_ppm,
                                                       g_device_settings.disp_s2_k_micro,
                                                       s_zero_cal_step1_avg2, avg2);
        s_zero_cal_phase = DISP_ZERO_CAL_RESULT_READY;
    }
}

/* Ends the precision measurement with a result (the Hann-weighted means of the
 * accepted 81-batch window, computed by math_precision_batch()). */
static void precision_succeed(void)
{
    s_precision_phase = DISP_PRECISION_DONE;
}

/* Ends the precision measurement with an error: no clean window within
 * DISPLACEMENT_PRECISION_TIMEOUT_MS. No value is reported. */
static void precision_fail(void)
{
    s_prec.result[0] = s_prec.result[1] = 0.0f;
    s_prec.failed = true;
    s_precision_phase = DISP_PRECISION_DONE;
}

/* Called once per batch, after the display stream was fed. */
static void precision_batch(const MathDisplay *ds)
{
    if (s_precision_phase != DISP_PRECISION_RUNNING) {
        return;
    }
    bool timed_out = (uint32_t)(hal_systick_get_ms() - s_precision_start_ms) >= DISPLACEMENT_PRECISION_TIMEOUT_MS;
    switch (math_precision_batch(&s_prec, ds, timed_out)) {
        case MATH_PRECISION_OK:      precision_succeed(); break;
        case MATH_PRECISION_TIMEOUT: precision_fail();    break;
        default:                     break;
    }
}

void disp_procs_init(void)
{
    math_precision_start(&s_prec);
}

void disp_procs_break(void)
{
    math_precision_break(&s_prec);
}

void disp_procs_batch(const MathDisplay *ds, float delta1_mm, float delta2_mm)
{
    zero_cal_accumulate(delta1_mm, delta2_mm);
    precision_batch(ds);
}

void disp_procs_poll(void)
{
    if (s_precision_phase == DISP_PRECISION_RUNNING
        && (uint32_t)(hal_systick_get_ms() - s_precision_start_ms) >= DISPLACEMENT_PRECISION_TIMEOUT_MS) {
        precision_fail();
    }
}

DrvStatus svc_displacement_zero_cal_step1_begin(uint8_t sensor_mask)
{
    if ((sensor_mask & ZERO_CAL_SENSORS_BOTH) == 0U) {
        return DRV_ERR_INVALID;
    }
    if (!svc_displacement_is_running()) {
        return DRV_ERR_NOT_READY;
    }
    if (s_zero_cal_phase != DISP_ZERO_CAL_IDLE
        && s_zero_cal_phase != DISP_ZERO_CAL_RESULT_READY) {
        return DRV_ERR_NOT_READY;   /* already mid-run -- cancel first */
    }
    /* Mutually exclusive with an in-progress precision measurement -- the
     * reverse check svc_displacement_precision_begin() already does.
     * Without this, both could run concurrently: harmless to memory (each
     * has its own sum/count state) but semantically wrong -- a zero-cal's
     * 180-degree flip mid-precision-measurement would silently corrupt
     * that measurement's average, and the two features' own doc comments
     * both claim this exclusivity, so it needs to actually hold in both
     * directions. */
    if (s_precision_phase == DISP_PRECISION_RUNNING) {
        return DRV_ERR_NOT_READY;
    }
    s_zero_cal_mask  = (uint8_t)(sensor_mask & ZERO_CAL_SENSORS_BOTH);
    s_zero_cal_sum1  = s_zero_cal_sum2  = 0.0f;
    s_zero_cal_count = 0;
    s_zero_cal_phase = DISP_ZERO_CAL_STEP1_RUNNING;
    return DRV_OK;
}

DrvStatus svc_displacement_zero_cal_step2_begin(void)
{
    if (!svc_displacement_is_running()) {
        return DRV_ERR_NOT_READY;
    }
    if (s_zero_cal_phase != DISP_ZERO_CAL_STEP1_DONE) {
        return DRV_ERR_NOT_READY;   /* step 1 hasn't finished (or wasn't started) */
    }
    s_zero_cal_sum1  = s_zero_cal_sum2  = 0.0f;
    s_zero_cal_count = 0;
    s_zero_cal_phase = DISP_ZERO_CAL_STEP2_RUNNING;
    return DRV_OK;
}

void svc_displacement_zero_cal_cancel(void)
{
    s_zero_cal_phase = DISP_ZERO_CAL_IDLE;
    s_zero_cal_count = 0;
}

DisplacementZeroCalPhase svc_displacement_zero_cal_get_phase(void)
{
    return s_zero_cal_phase;
}

void svc_displacement_zero_cal_progress(uint16_t *count_out, uint16_t *target_out)
{
    if (count_out)  *count_out  = s_zero_cal_count;
    if (target_out) *target_out = DISPLACEMENT_ZERO_CAL_SAMPLES;
}

uint8_t svc_displacement_zero_cal_get_mask(void)
{
    return s_zero_cal_mask;
}

bool svc_displacement_zero_cal_consume_result(float *offset1_mm_out, float *offset2_mm_out,
                                               uint8_t *sensor_mask_out)
{
    if (s_zero_cal_phase != DISP_ZERO_CAL_RESULT_READY) {
        return false;
    }
    if (offset1_mm_out) *offset1_mm_out = s_zero_cal_result1_mm;
    if (offset2_mm_out) *offset2_mm_out = s_zero_cal_result2_mm;
    if (sensor_mask_out) *sensor_mask_out = s_zero_cal_mask;
    s_zero_cal_phase = DISP_ZERO_CAL_IDLE;
    return true;
}

DrvStatus svc_displacement_precision_begin(void)
{
    if (!svc_displacement_is_running()) {
        return DRV_ERR_NOT_READY;
    }
    if (s_zero_cal_phase != DISP_ZERO_CAL_IDLE
        && s_zero_cal_phase != DISP_ZERO_CAL_RESULT_READY) {
        return DRV_ERR_NOT_READY;   /* mutually exclusive with an in-progress zero-cal */
    }
    math_precision_start(&s_prec);
    s_precision_start_ms   = hal_systick_get_ms();
    s_precision_phase      = DISP_PRECISION_RUNNING;
    return DRV_OK;
}

void svc_displacement_precision_cancel(void)
{
    s_precision_phase     = DISP_PRECISION_IDLE;
    s_prec.fill           = 0;
    s_prec.disturbed      = false;
}

DisplacementPrecisionPhase svc_displacement_precision_get_phase(void)
{
    return s_precision_phase;
}

bool svc_displacement_precision_get_disturbed(void)
{
    return s_precision_phase == DISP_PRECISION_RUNNING && s_prec.disturbed;
}

void svc_displacement_precision_progress(uint16_t *count1_out, uint16_t *count2_out,
                                          uint16_t *count_diff_out, uint16_t *target_out,
                                          uint32_t *elapsed_ms_out)
{
    /* All three counts report the same thing now (the window fill since the
     * trigger); the three fields are kept for wire compatibility. */
    if (count1_out)     *count1_out     = s_prec.fill;
    if (count2_out)     *count2_out     = s_prec.fill;
    if (count_diff_out) *count_diff_out = s_prec.fill;
    if (target_out)     *target_out     = DISPLACEMENT_PRECISION_WINDOW_BATCHES;
    if (elapsed_ms_out) {
        *elapsed_ms_out = (s_precision_phase == DISP_PRECISION_IDLE)
            ? 0U : (uint32_t)(hal_systick_get_ms() - s_precision_start_ms);
    }
}

bool svc_displacement_precision_get_result(float *delta1_mm_out, float *delta2_mm_out,
                                            float *delta_diff_mm_out, bool *failed_out)
{
    if (s_precision_phase != DISP_PRECISION_DONE) {
        return false;
    }
    /* Same sign-flip-at-the-output-boundary policy as the getters above --
     * the window means are in the sensors' native sign convention. */
    float d1 = disp_sensor_sign(g_device_settings.disp_s1_invert) * s_prec.result[0];
    float d2 = disp_sensor_sign(g_device_settings.disp_s2_invert) * s_prec.result[1];
    if (delta1_mm_out)      *delta1_mm_out      = d1;
    if (delta2_mm_out)      *delta2_mm_out      = d2;
    if (delta_diff_mm_out)  *delta_diff_mm_out  = d1 - d2;   /* correct for any combination of invert flags */
    if (failed_out)         *failed_out         = s_prec.failed;
    return true;
}
