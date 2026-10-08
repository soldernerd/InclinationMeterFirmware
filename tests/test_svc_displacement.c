/* Host tests for the zero (flip) calibration and the precision measurement of Services/svc_displacement.c -- the REAL
 * source is included, with the ADC driver and the clock replaced by doubles, and the real Math/ sources linked in. The
 * tests feed it synthetic carrier samples through the same entry point the ADC drain uses (on_sample), so they cover
 * the whole chain: accumulation, batch demodulation, calibration, the two phase machines and their exclusivity. */
#include "test.h"
#include <math.h>
#include <string.h>

#include "../system_state.h"
#include "../Config/config.h"
#include "../Drivers_App/drv_ads131m04.h"
#include "../Services/svc_log.h"

/* ---------------- doubles ---------------- */
DeviceSettings g_device_settings;
SystemState    g_system_state;

static uint32_t g_ms;
uint32_t hal_systick_get_ms(void) { return g_ms; }

static bool g_running;
static Ads131m04SampleCb g_cb;
static Ads131m04Integrity g_integrity;
DrvStatus drv_ads131m04_init(void) { g_cb = 0; return DRV_OK; }
DrvStatus drv_ads131m04_start(void) { g_running = true; return DRV_OK; }
void      drv_ads131m04_stop(void) { g_running = false; }
bool      drv_ads131m04_is_running(void) { return g_running; }
void      drv_ads131m04_set_on_sample(Ads131m04SampleCb cb) { g_cb = cb; }
bool      drv_ads131m04_faulted(void) { return false; }
const volatile Ads131m04Integrity *drv_ads131m04_get_integrity(void) { return &g_integrity; }
void svc_log(Api2LogSeverity s, const char *m) { (void)s; (void)m; }
void svc_logf(Api2LogSeverity s, const char *f, ...) { (void)s; (void)f; }

#include "../Math/math_phasor.c"
#include "../Math/math_window.c"
#include "../Math/math_displacement.c"
#include "../Math/math_quality.c"
#include "../Services/svc_displacement.c"
#include "../Services/svc_disp_capture.c"
#include "../Services/svc_disp_phasor_stream.c"
#include "../Services/svc_disp_procedures.c"

/* ---------------- helpers ---------------- */
#define EXC_AMP   400000.0                    /* A = +EXC_AMP, B = -EXC_AMP: D = A - B = 2 EXC_AMP */
#define PI_D      3.14159265358979323846

/* One carrier cycle of one channel: amp * cos(n 45deg - phi). */
static int32_t sample(double amp, double phi, int n)
{
    return (int32_t)lround(amp * cos((double)n * PI_D / 4.0 - phi));
}

/* The signal amplitude that makes sensor reading `r` (mm/m, with k = 1 and zero = 0) at its PGA. */
static double sensor_amp(double r) { return r * (double)ADS131M04_PGA_S1 * 2.0 * EXC_AMP; }

/* Feeds one batch (DISPLACEMENT_BATCH_CYCLES cycles) the way the ADC drain does, then runs the task. */
static void feed_batch_phase(double r1, double phi1, double r2, double phi2)
{
    for (unsigned c = 0; c < DISPLACEMENT_BATCH_CYCLES; ++c) {
        for (int n = 0; n < 8; ++n) {
            on_sample(sample(sensor_amp(r2), phi2, n),      /* ch0 = S2 */
                      sample(-EXC_AMP, 0.0, n),             /* ch1 = B  */
                      sample(EXC_AMP, 0.0, n),              /* ch2 = A  */
                      sample(sensor_amp(r1), phi1, n));     /* ch3 = S1 */
        }
    }
    g_ms += 25;
    svc_displacement_update();
}
static void feed_batch(double r1, double r2) { feed_batch_phase(r1, 0.0, r2, 0.0); }
static void feed(unsigned n, double r1, double r2) { for (unsigned i = 0; i < n; ++i) feed_batch(r1, r2); }

/* Batches whose sensor-1 carrier phase alternates: large steps of the quadrature residual -- a "disturbed" window. */
static void feed_disturbed(unsigned n, double r1, double r2)
{
    for (unsigned i = 0; i < n; ++i) feed_batch_phase(r1, (i & 1U) ? 0.4 : -0.4, r2, 0.0);
}

static void fresh(void)
{
    memset(&g_device_settings, 0, sizeof g_device_settings);
    g_device_settings.disp_s1_k_micro = 1000000;   /* k = 1 per (mm/m) at PGA 1: reading = ratio */
    g_device_settings.disp_s2_k_micro = 1000000;
    memset(&s_phase_cache, 0, sizeof s_phase_cache);
    g_running = false;
    g_ms = 100000;
    svc_displacement_init();
    CHECK_EQ(svc_displacement_start(), DRV_OK);
    CHECK(svc_displacement_is_running());
}

/* A quiet start: enough clean batches that the quality floor is established (two full windows), so the precision
 * measurement can reach a verdict. The signal is perfectly stable, so the floor is exactly zero. */
static void warm_up(double r1, double r2) { feed(2U * DISPLACEMENT_PRECISION_WINDOW_BATCHES + 10U, r1, r2); }

#define NEAR(a, b, tol) CHECK(fabsf((float)(a) - (float)(b)) <= (float)(tol))

/* ---------------- the synthetic chain itself ---------------- */
TEST(the_synthetic_signal_reads_back_as_the_ratio_it_was_built_from)
{
    fresh();
    feed(5, 0.0125, -0.0300);
    CHECK(svc_displacement_get_ok());
    NEAR(s_delta1_mm_raw, 0.0125, 1e-5);
    NEAR(s_delta2_mm_raw, -0.0300, 1e-5);
    NEAR(s_residual1, 0.0, 1e-5);
}

/* ---------------- zero calibration ---------------- */
TEST(zero_cal_refuses_bad_calls)
{
    fresh();
    CHECK_EQ(svc_displacement_zero_cal_step1_begin(0), DRV_ERR_INVALID);
    CHECK_EQ(svc_displacement_zero_cal_step1_begin(0xFC), DRV_ERR_INVALID);        /* no valid sensor bit */
    CHECK_EQ(svc_displacement_zero_cal_step2_begin(), DRV_ERR_NOT_READY);          /* step 1 never ran */
    float a, b; uint8_t m;
    CHECK(!svc_displacement_zero_cal_consume_result(&a, &b, &m));                  /* nothing to consume */
    drv_ads131m04_stop();
    CHECK_EQ(svc_displacement_zero_cal_step1_begin(ZERO_CAL_SENSORS_BOTH), DRV_ERR_NOT_READY);   /* acquisition off */
    CHECK_EQ(svc_displacement_zero_cal_get_phase(), DISP_ZERO_CAL_IDLE);
}

TEST(zero_cal_two_orientations_give_the_instrument_zero)
{
    fresh();
    g_device_settings.disp_s1_zero_ppm = 500;                    /* an existing zero: 0.0005 mm/m at k = 1 */
    g_device_settings.disp_s2_zero_ppm = -200;
    const double tilt1 = 0.0300, e1 = 0.0020;                    /* real tilt, instrument zero error */
    const double tilt2 = -0.0120, e2 = -0.0007;
    feed(3, 0.0, 0.0);

    CHECK_EQ(svc_displacement_zero_cal_step1_begin(ZERO_CAL_SENSORS_BOTH), DRV_OK);
    CHECK_EQ(svc_displacement_zero_cal_get_phase(), DISP_ZERO_CAL_STEP1_RUNNING);
    CHECK_EQ(svc_displacement_zero_cal_step1_begin(ZERO_CAL_SENSORS_BOTH), DRV_ERR_NOT_READY);   /* already running */
    CHECK_EQ(svc_displacement_zero_cal_step2_begin(), DRV_ERR_NOT_READY);                        /* step 1 not finished */

    /* what a sensor shows: tilt + zero error (the stored zero is removed by the firmware before display) */
    feed(DISPLACEMENT_ZERO_CAL_SAMPLES - 1U, tilt1 + e1 + 0.0005, tilt2 + e2 - 0.0002);
    uint16_t cnt, tgt;
    svc_displacement_zero_cal_progress(&cnt, &tgt);
    CHECK_EQ(cnt, DISPLACEMENT_ZERO_CAL_SAMPLES - 1U);
    CHECK_EQ(tgt, DISPLACEMENT_ZERO_CAL_SAMPLES);
    CHECK_EQ(svc_displacement_zero_cal_get_phase(), DISP_ZERO_CAL_STEP1_RUNNING);
    feed(1, tilt1 + e1 + 0.0005, tilt2 + e2 - 0.0002);
    CHECK_EQ(svc_displacement_zero_cal_get_phase(), DISP_ZERO_CAL_STEP1_DONE);

    feed(10, 0.0, 0.0);                                           /* the operator turns the instrument: nothing counts */
    CHECK_EQ(svc_displacement_zero_cal_get_phase(), DISP_ZERO_CAL_STEP1_DONE);
    CHECK_EQ(svc_displacement_zero_cal_step2_begin(), DRV_OK);
    CHECK_EQ(svc_displacement_zero_cal_get_phase(), DISP_ZERO_CAL_STEP2_RUNNING);
    feed(DISPLACEMENT_ZERO_CAL_SAMPLES, -tilt1 + e1 + 0.0005, -tilt2 + e2 - 0.0002);   /* reversed: the tilt flips, the error does not */
    CHECK_EQ(svc_displacement_zero_cal_get_phase(), DISP_ZERO_CAL_RESULT_READY);

    float o1 = 0, o2 = 0; uint8_t mask = 0;
    CHECK(svc_displacement_zero_cal_consume_result(&o1, &o2, &mask));
    NEAR(o1, e1 + 0.0005, 2e-5);                                  /* the new ABSOLUTE zero: old zero + the residual error */
    NEAR(o2, e2 - 0.0002, 2e-5);
    CHECK_EQ(mask, ZERO_CAL_SENSORS_BOTH);
    CHECK_EQ(svc_displacement_zero_cal_get_phase(), DISP_ZERO_CAL_IDLE);
    CHECK(!svc_displacement_zero_cal_consume_result(&o1, &o2, &mask));   /* consumed once */
}

TEST(zero_cal_can_be_cancelled_and_restarted)
{
    fresh();
    feed(2, 0.0, 0.0);
    CHECK_EQ(svc_displacement_zero_cal_step1_begin(ZERO_CAL_SENSORS_BOTH), DRV_OK);
    feed(10, 0.01, 0.01);
    svc_displacement_zero_cal_cancel();
    CHECK_EQ(svc_displacement_zero_cal_get_phase(), DISP_ZERO_CAL_IDLE);
    feed(100, 0.5, 0.5);                                          /* idle: nothing accumulates, nothing completes */
    CHECK_EQ(svc_displacement_zero_cal_get_phase(), DISP_ZERO_CAL_IDLE);
    CHECK_EQ(svc_displacement_zero_cal_step1_begin(ZERO_CAL_SENSORS_BOTH), DRV_OK);   /* and a new run starts from zero */
    uint16_t cnt; svc_displacement_zero_cal_progress(&cnt, 0);
    CHECK_EQ(cnt, 0);
}

TEST(a_stop_cancels_a_calibration_that_is_in_progress)
{
    fresh();
    feed(2, 0.0, 0.0);
    CHECK_EQ(svc_displacement_zero_cal_step1_begin(ZERO_CAL_SENSORS_BOTH), DRV_OK);
    feed(DISPLACEMENT_ZERO_CAL_SAMPLES, 0.02, 0.02);
    CHECK_EQ(svc_displacement_zero_cal_get_phase(), DISP_ZERO_CAL_STEP1_DONE);
    svc_displacement_stop();                                      /* step 2 must never combine data from another session */
    CHECK_EQ(svc_displacement_zero_cal_get_phase(), DISP_ZERO_CAL_IDLE);
    CHECK_EQ(svc_displacement_start(), DRV_OK);
    CHECK_EQ(svc_displacement_zero_cal_step2_begin(), DRV_ERR_NOT_READY);
}

TEST(a_sensor_mask_is_remembered_for_the_result)
{
    fresh();
    feed(2, 0.0, 0.0);
    CHECK_EQ(svc_displacement_zero_cal_step1_begin(0x01), DRV_OK);
    CHECK_EQ(svc_displacement_zero_cal_get_mask(), 0x01);
}

/* ---------------- precision measurement ---------------- */
TEST(precision_needs_the_acquisition_running)
{
    fresh();
    drv_ads131m04_stop();
    CHECK_EQ(svc_displacement_precision_begin(), DRV_ERR_NOT_READY);
    CHECK_EQ(svc_displacement_precision_get_phase(), DISP_PRECISION_IDLE);
}

TEST(precision_returns_the_window_mean_of_a_steady_reading)
{
    fresh();
    warm_up(0.0123, -0.0045);
    CHECK_EQ(svc_displacement_precision_begin(), DRV_OK);
    CHECK_EQ(svc_displacement_precision_get_phase(), DISP_PRECISION_RUNNING);
    float d1, d2, dd; bool failed;
    CHECK(!svc_displacement_precision_get_result(&d1, &d2, &dd, &failed));   /* nothing yet */

    feed(DISPLACEMENT_PRECISION_WINDOW_BATCHES - 1U, 0.0123, -0.0045);
    uint16_t c1, c2, cd, tgt; uint32_t el;
    svc_displacement_precision_progress(&c1, &c2, &cd, &tgt, &el);
    CHECK_EQ(c1, DISPLACEMENT_PRECISION_WINDOW_BATCHES - 1U);
    CHECK_EQ(tgt, DISPLACEMENT_PRECISION_WINDOW_BATCHES);
    CHECK(el > 0U);
    CHECK_EQ(svc_displacement_precision_get_phase(), DISP_PRECISION_RUNNING);

    feed(1, 0.0123, -0.0045);                                     /* the window is full and clean */
    CHECK_EQ(svc_displacement_precision_get_phase(), DISP_PRECISION_DONE);
    CHECK(svc_displacement_precision_get_result(&d1, &d2, &dd, &failed));
    CHECK(!failed);
    NEAR(d1, 0.0123, 1e-5);
    NEAR(d2, -0.0045, 1e-5);
    NEAR(dd, 0.0123 + 0.0045, 2e-5);
    CHECK(svc_displacement_precision_get_result(&d1, &d2, &dd, &failed));   /* the result stays until cancelled */
    svc_displacement_precision_cancel();
    CHECK_EQ(svc_displacement_precision_get_phase(), DISP_PRECISION_IDLE);
    CHECK(!svc_displacement_precision_get_result(&d1, &d2, &dd, &failed));
}

TEST(the_invert_flags_flip_the_reported_precision_result)
{
    fresh();
    g_device_settings.disp_s1_invert = 1;
    warm_up(0.0200, 0.0100);
    CHECK_EQ(svc_displacement_precision_begin(), DRV_OK);
    feed(DISPLACEMENT_PRECISION_WINDOW_BATCHES, 0.0200, 0.0100);
    float d1, d2, dd; bool failed;
    CHECK(svc_displacement_precision_get_result(&d1, &d2, &dd, &failed));
    NEAR(d1, -0.0200, 1e-5);                                      /* S1 inverted, S2 not */
    NEAR(d2, 0.0100, 1e-5);
    NEAR(dd, -0.0300, 2e-5);                                      /* the difference follows the flipped signs */
}

TEST(a_disturbed_window_is_not_accepted_and_the_measurement_times_out)
{
    fresh();
    warm_up(0.01, 0.01);
    CHECK_EQ(svc_displacement_precision_begin(), DRV_OK);
    feed_disturbed(DISPLACEMENT_PRECISION_WINDOW_BATCHES + 20U, 0.01, 0.01);
    CHECK_EQ(svc_displacement_precision_get_phase(), DISP_PRECISION_RUNNING);   /* no verdict: waiting for quiet */
    CHECK(svc_displacement_precision_get_disturbed());

    g_ms += DISPLACEMENT_PRECISION_TIMEOUT_MS;                    /* the time budget is used up */
    feed_disturbed(1, 0.01, 0.01);
    CHECK_EQ(svc_displacement_precision_get_phase(), DISP_PRECISION_DONE);
    float d1 = 1, d2 = 1, dd = 1; bool failed = false;
    CHECK(svc_displacement_precision_get_result(&d1, &d2, &dd, &failed));
    CHECK(failed);
    NEAR(d1, 0.0, 1e-9);                                          /* an error reports no value at all */
    NEAR(d2, 0.0, 1e-9);
    CHECK(!svc_displacement_precision_get_disturbed());
}

TEST(the_timeout_fires_even_when_no_batch_arrives)
{
    fresh();
    warm_up(0.01, 0.01);
    CHECK_EQ(svc_displacement_precision_begin(), DRV_OK);
    const uint32_t t0 = g_ms;
    feed(10, 0.01, 0.01);
    g_ms = t0 + DISPLACEMENT_PRECISION_TIMEOUT_MS - 1U;
    svc_displacement_update();
    CHECK_EQ(svc_displacement_precision_get_phase(), DISP_PRECISION_RUNNING);   /* not yet */
    g_ms += 1U;
    svc_displacement_update();                                    /* acquisition stalled: the task still ends it */
    CHECK_EQ(svc_displacement_precision_get_phase(), DISP_PRECISION_DONE);
    float d1, d2, dd; bool failed = false;
    CHECK(svc_displacement_precision_get_result(&d1, &d2, &dd, &failed));
    CHECK(failed);
}

TEST(a_dropped_batch_restarts_the_precision_window)
{
    fresh();
    warm_up(0.01, 0.01);
    CHECK_EQ(svc_displacement_precision_begin(), DRV_OK);
    feed(40, 0.01, 0.01);
    uint16_t c1, c2, cd, tgt; uint32_t el;
    svc_displacement_precision_progress(&c1, &c2, &cd, &tgt, &el);
    CHECK_EQ(c1, 40);
    s_cycle_seq = (uint16_t)(s_cycle_seq + DISPLACEMENT_BATCH_CYCLES);   /* a whole batch was lost on the way */
    feed(1, 0.01, 0.01);
    svc_displacement_precision_progress(&c1, &c2, &cd, &tgt, &el);
    CHECK(c1 <= 1U);                                              /* no window may span the gap */
    feed(DISPLACEMENT_PRECISION_WINDOW_BATCHES - 1U, 0.01, 0.01);
    CHECK_EQ(svc_displacement_precision_get_phase(), DISP_PRECISION_DONE);
}

TEST(precision_and_zero_cal_exclude_each_other_in_both_directions)
{
    fresh();
    warm_up(0.0, 0.0);
    CHECK_EQ(svc_displacement_zero_cal_step1_begin(ZERO_CAL_SENSORS_BOTH), DRV_OK);
    CHECK_EQ(svc_displacement_precision_begin(), DRV_ERR_NOT_READY);       /* zero-cal running */
    svc_displacement_zero_cal_cancel();
    CHECK_EQ(svc_displacement_precision_begin(), DRV_OK);
    CHECK_EQ(svc_displacement_zero_cal_step1_begin(ZERO_CAL_SENSORS_BOTH), DRV_ERR_NOT_READY);   /* precision running */
    svc_displacement_precision_cancel();
    CHECK_EQ(svc_displacement_zero_cal_step1_begin(ZERO_CAL_SENSORS_BOTH), DRV_OK);
}

TEST(a_finished_precision_run_does_not_block_a_zero_calibration)
{
    fresh();
    warm_up(0.0, 0.0);
    CHECK_EQ(svc_displacement_precision_begin(), DRV_OK);
    feed(DISPLACEMENT_PRECISION_WINDOW_BATCHES, 0.0, 0.0);
    CHECK_EQ(svc_displacement_precision_get_phase(), DISP_PRECISION_DONE);
    CHECK_EQ(svc_displacement_zero_cal_step1_begin(ZERO_CAL_SENSORS_BOTH), DRV_OK);
}

TEST(a_stop_cancels_a_running_precision_measurement)
{
    fresh();
    warm_up(0.0, 0.0);
    CHECK_EQ(svc_displacement_precision_begin(), DRV_OK);
    svc_displacement_stop();
    CHECK_EQ(svc_displacement_precision_get_phase(), DISP_PRECISION_IDLE);
}

int main(void)
{
    RUN(the_synthetic_signal_reads_back_as_the_ratio_it_was_built_from);
    RUN(zero_cal_refuses_bad_calls);
    RUN(zero_cal_two_orientations_give_the_instrument_zero);
    RUN(zero_cal_can_be_cancelled_and_restarted);
    RUN(a_stop_cancels_a_calibration_that_is_in_progress);
    RUN(a_sensor_mask_is_remembered_for_the_result);
    RUN(precision_needs_the_acquisition_running);
    RUN(precision_returns_the_window_mean_of_a_steady_reading);
    RUN(the_invert_flags_flip_the_reported_precision_result);
    RUN(a_disturbed_window_is_not_accepted_and_the_measurement_times_out);
    RUN(the_timeout_fires_even_when_no_batch_arrives);
    RUN(a_dropped_batch_restarts_the_precision_window);
    RUN(precision_and_zero_cal_exclude_each_other_in_both_directions);
    RUN(a_finished_precision_run_does_not_block_a_zero_calibration);
    RUN(a_stop_cancels_a_running_precision_measurement);
    return test_summary();
}
