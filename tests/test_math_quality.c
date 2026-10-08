/* Host tests for Math/math_quality.c -- the display stream and the sliding-window precision measurement.
 * Synthetic readings with a controllable residual; the replay on recorded data is test_golden_displacement.c. */
#include "test.h"
#include <math.h>
#include <string.h>

#include "../Math/math_window.c"
#include "../Math/math_quality.c"

#define STEP   64U       /* cycles per batch */
#define TAPS   25U
#define DECIM  10U
#define PREC   81U
#define K      6.0f
#define CREEP  1.42e-5f
#define RESEED 100000u   /* far away: these tests are not about the re-seed rule */

static MathDisplay ds;
static MathPrecision pr;
static uint16_t g_seq;

static void fresh(void)
{
    math_display_init(&ds, TAPS, DECIM, PREC, K, CREEP, RESEED);
    math_precision_start(&pr);
    g_seq = 1000;
}

/* One batch: both sensors read `v`, the residual alternates +-amp (a constant squared step 4*amp^2). */
static bool feed(float v1, float v2, float amp)
{
    static int sign = 1;
    sign = -sign;
    float delta[2] = { v1, v2 };
    float res[2]   = { sign * amp, sign * amp };
    bool contiguous = math_display_feed(&ds, g_seq, STEP, delta, res);
    g_seq = (uint16_t)(g_seq + STEP);
    return contiguous;
}

static void feed_n(int n, float v1, float v2, float amp)
{
    for (int i = 0; i < n; ++i) feed(v1, v2, amp);
}

TEST(nothing_is_published_until_the_display_window_is_full)
{
    fresh();
    CHECK(!ds.valid);
    uint16_t seq0 = ds.seq;
    feed_n(24, 1.0f, 2.0f, 1e-4f);
    CHECK(!ds.valid);
    CHECK_EQ(ds.seq, seq0);
}

TEST(the_first_value_appears_at_the_first_decimation_point_after_the_window_fills)
{
    fresh();
    uint16_t seq0 = ds.seq;
    feed_n(29, 1.0f, 2.0f, 1e-4f);
    CHECK(!ds.valid);                    /* 10 and 20 came before the window (25) was full */
    feed(1.0f, 2.0f, 1e-4f);             /* batch 30 */
    CHECK(ds.valid);
    CHECK_EQ(ds.seq, seq0 + 1);
    CHECK(fabsf(ds.out[0] - 1.0f) < 1e-6f && fabsf(ds.out[1] - 2.0f) < 1e-6f);   /* Hann mean of a constant */
    feed_n(9, 1.0f, 2.0f, 1e-4f);
    CHECK_EQ(ds.seq, seq0 + 1);          /* one publish per 10 batches */
    feed(1.0f, 2.0f, 1e-4f);
    CHECK_EQ(ds.seq, seq0 + 2);
}

TEST(the_hann_window_weights_the_newest_readings_symmetrically)
{
    fresh();
    feed_n(40, 0.0f, 0.0f, 1e-4f);
    /* a step 12 batches ago sits at tap 12 of 25 -- the window centre carries the largest weight */
    feed_n(1, 1.0f, 1.0f, 1e-4f);
    feed_n(12, 0.0f, 0.0f, 1e-4f);
    feed_n(7, 0.0f, 0.0f, 1e-4f);        /* 60 batches in total: a publish happens at 60 */
    CHECK(ds.out[0] > 0.0f && ds.out[0] < 0.15f);     /* a single unit impulse: weight w/sum, < 1/12 or so */
}

TEST(quiet_data_is_not_doubtful_after_the_floor_is_established)
{
    fresh();
    feed_n(400, 0.5f, 0.5f, 1e-4f);
    CHECK(ds.valid);
    CHECK(!ds.doubtful[0] && !ds.doubtful[1]);
}

TEST(a_burst_is_flagged_doubtful_and_recovers)
{
    fresh();
    feed_n(400, 0.5f, 0.5f, 1e-4f);
    feed_n(40, 0.5f, 0.5f, 1e-2f);       /* 100x larger residual steps: 10^4 x the power */
    CHECK(ds.doubtful[0] && ds.doubtful[1]);
    CHECK(math_window_floor_ready(&ds.win));
    feed_n(100, 0.5f, 0.5f, 1e-4f);
    CHECK(!ds.doubtful[0] && !ds.doubtful[1]);
}

TEST(the_value_is_published_even_when_doubtful)
{
    fresh();
    feed_n(400, 0.5f, 0.5f, 1e-4f);
    uint16_t seq0 = ds.seq;
    feed_n(30, 0.9f, 0.9f, 1e-2f);
    CHECK(ds.doubtful[0]);
    CHECK(ds.seq > seq0);
    CHECK(ds.out[0] > 0.5f);             /* shown anyway */
}

TEST(a_dropped_batch_forgets_the_window)
{
    fresh();
    feed_n(60, 1.0f, 1.0f, 1e-4f);
    CHECK(ds.win.count >= TAPS);
    g_seq = (uint16_t)(g_seq + STEP);                    /* one batch missing */
    bool contiguous = feed(1.0f, 1.0f, 1e-4f);
    CHECK(!contiguous);
    CHECK_EQ(ds.win.count, 1);                           /* only the batch after the gap */
    CHECK(feed(1.0f, 1.0f, 1e-4f));                      /* and the series continues from there */
}

TEST(the_sequence_counter_wraps_without_a_false_gap)
{
    fresh();
    g_seq = (uint16_t)(65536u - 3u * STEP);
    for (int i = 0; i < 8; ++i) {
        bool c = feed(1.0f, 1.0f, 1e-4f);
        if (i > 0) CHECK(c);             /* 65536 wraps to 0 -- modular arithmetic */
    }
}

TEST(the_first_batch_after_a_start_counts_as_a_break)
{
    fresh();
    CHECK(!feed(1.0f, 1.0f, 1e-4f));
    CHECK(feed(1.0f, 1.0f, 1e-4f));
}

TEST(skip_invalidates_the_stream_and_the_next_batch_starts_a_new_window)
{
    fresh();
    feed_n(60, 1.0f, 1.0f, 1e-4f);
    CHECK(ds.valid);
    math_display_skip(&ds);
    CHECK(!ds.valid);
    CHECK(!feed(1.0f, 1.0f, 1e-4f));
}

TEST(reset_clears_everything_but_keeps_counting_seq)
{
    fresh();
    feed_n(400, 1.0f, 1.0f, 1e-4f);
    uint16_t seq = ds.seq;
    math_display_reset(&ds);
    CHECK(!ds.valid && !ds.doubtful[0] && !ds.doubtful[1]);
    CHECK(ds.out[0] == 0.0f && ds.out[1] == 0.0f);
    CHECK_EQ(ds.win.count, 0);
    CHECK(!math_window_floor_ready(&ds.win));
    CHECK_EQ(ds.seq, seq + 1);           /* a consumer sees the change */
}

/* ---------------- precision ---------------- */

TEST(precision_waits_for_the_floor_then_accepts_the_first_clean_window)
{
    fresh();
    int ok_at = -1;
    for (int i = 1; i <= 400; ++i) {
        feed(0.7f, -0.2f, 1e-4f);
        MathPrecisionStep st = math_precision_batch(&pr, &ds, false);
        if (st == MATH_PRECISION_OK) { ok_at = i; break; }
        CHECK(st == MATH_PRECISION_WAITING);
    }
    CHECK(ok_at > (int)PREC);            /* never before a full window ... */
    CHECK(math_window_floor_ready(&ds.win));   /* ... and only once the quiet floor can be judged */
    CHECK(ok_at <= 2 * (int)PREC + 3);
    CHECK(fabsf(pr.result[0] - 0.7f) < 1e-5f && fabsf(pr.result[1] + 0.2f) < 1e-5f);
    CHECK(!pr.failed && !pr.disturbed);
}

TEST(precision_does_not_accept_a_disturbed_window_and_reports_it)
{
    fresh();
    feed_n(400, 0.0f, 0.0f, 1e-4f);      /* quiet history: the floor is known */
    math_precision_start(&pr);
    int ok_at = -1;
    int disturbed_seen = 0;
    for (int i = 1; i <= 400; ++i) {
        feed(0.3f, 0.3f, i < 120 ? 1e-2f : 1e-4f);     /* disturbance for the first 120 batches */
        MathPrecisionStep st = math_precision_batch(&pr, &ds, false);
        if (pr.disturbed) disturbed_seen = 1;
        if (st == MATH_PRECISION_OK) { ok_at = i; break; }
    }
    CHECK(disturbed_seen);
    CHECK(ok_at > 120 + (int)PREC - 5);  /* the window must have left the disturbance completely */
    CHECK(ok_at < 400);
}

TEST(precision_times_out_without_a_value_when_never_clean)
{
    fresh();
    feed_n(400, 0.0f, 0.0f, 1e-4f);
    math_precision_start(&pr);
    MathPrecisionStep st = MATH_PRECISION_WAITING;
    for (int i = 1; i <= 200 && st == MATH_PRECISION_WAITING; ++i) {
        feed(0.3f, 0.3f, 1e-2f);
        st = math_precision_batch(&pr, &ds, i >= 150);
    }
    CHECK(st == MATH_PRECISION_TIMEOUT);
    CHECK(pr.failed);
    CHECK(pr.result[0] == 0.0f && pr.result[1] == 0.0f);
}

TEST(a_clean_window_wins_over_a_simultaneous_timeout)
{
    fresh();
    feed_n(400, 0.0f, 0.0f, 1e-4f);
    math_precision_start(&pr);
    MathPrecisionStep st = MATH_PRECISION_WAITING;
    for (int i = 1; i <= (int)PREC; ++i) {
        feed(0.3f, 0.3f, 1e-4f);
        /* the budget runs out exactly when the first full window arrives */
        st = math_precision_batch(&pr, &ds, i == (int)PREC);
        if (i < (int)PREC) CHECK(st == MATH_PRECISION_WAITING);
    }
    CHECK(st == MATH_PRECISION_OK);      /* a result beats the timeout, as in the firmware */
    CHECK(!pr.failed);
}

TEST(a_break_makes_the_measurement_fill_again)
{
    fresh();
    feed_n(400, 0.0f, 0.0f, 1e-4f);
    math_precision_start(&pr);
    for (int i = 0; i < 50; ++i) {
        feed(0.3f, 0.3f, 1e-4f);
        math_precision_batch(&pr, &ds, false);
    }
    CHECK_EQ(pr.fill, 50);
    g_seq = (uint16_t)(g_seq + STEP);                    /* a dropped batch */
    if (!feed(0.3f, 0.3f, 1e-4f)) math_precision_break(&pr);
    CHECK_EQ(pr.fill, 0);
    int ok_at = -1;
    for (int i = 1; i <= 200; ++i) {
        feed(0.3f, 0.3f, 1e-4f);
        if (math_precision_batch(&pr, &ds, false) == MATH_PRECISION_OK) { ok_at = i; break; }
    }
    CHECK(ok_at >= (int)PREC - 1);       /* a whole new window after the gap */
}

TEST(start_resets_a_previous_run)
{
    fresh();
    pr.fill = 70; pr.disturbed = true; pr.failed = true; pr.result[0] = 5.0f;
    math_precision_start(&pr);
    CHECK_EQ(pr.fill, 0);
    CHECK(!pr.disturbed && !pr.failed && pr.result[0] == 0.0f);
}

int main(void)
{
    RUN(nothing_is_published_until_the_display_window_is_full);
    RUN(the_first_value_appears_at_the_first_decimation_point_after_the_window_fills);
    RUN(the_hann_window_weights_the_newest_readings_symmetrically);
    RUN(quiet_data_is_not_doubtful_after_the_floor_is_established);
    RUN(a_burst_is_flagged_doubtful_and_recovers);
    RUN(the_value_is_published_even_when_doubtful);
    RUN(a_dropped_batch_forgets_the_window);
    RUN(the_sequence_counter_wraps_without_a_false_gap);
    RUN(the_first_batch_after_a_start_counts_as_a_break);
    RUN(skip_invalidates_the_stream_and_the_next_batch_starts_a_new_window);
    RUN(reset_clears_everything_but_keeps_counting_seq);
    RUN(precision_waits_for_the_floor_then_accepts_the_first_clean_window);
    RUN(precision_does_not_accept_a_disturbed_window_and_reports_it);
    RUN(precision_times_out_without_a_value_when_never_clean);
    RUN(a_clean_window_wins_over_a_simultaneous_timeout);
    RUN(a_break_makes_the_measurement_fill_again);
    RUN(start_resets_a_previous_run);
    return test_summary();
}
