#include "svc_displacement.h"
#include "drv_ads131m04.h"
#include "math_phasor.h"
#include "svc_log.h"
#include "hal_systick.h"
#include "config.h"
#include "system_state.h"
#include <stddef.h>
#include <stdbool.h>
#include <math.h>   /* sqrtf/atan2f -- signal diagnostics (2026-09-27), Services layer float use only */

/* Hot path: MUST be built optimised -- see CMakeLists.txt's pin and
 * Math/math_phasor.c's matching guard. on_sample() runs inside
 * Drivers_App/drv_ads131m04.c's SysTick frame drain, the same hot path
 * WP8's svc_signal_analysis.c (this module's predecessor) shared -O2
 * with. */
#if !defined(__OPTIMIZE__)
#error "hot-path file built without optimisation -- restore the -O2 pin in CMakeLists.txt"
#endif

/* REV B channel mapping (confirmed with the user 2026-09-24 -- no
 * existing hardware doc states this, recorded here and in
 * svc_displacement.h): ch0=S2, ch1=B, ch2=A, ch3=S1. This is NOT the
 * mapping the never-merged wp10 (REV A) branch used (ch0=B, ch1=A,
 * ch2=S1, ch3=S2) -- REV B's front end is wired differently; only the
 * demodulation math below is carried over unchanged.
 *
 * Derivation of x from the measured ADC-domain phasors: the ADC sees
 * S_adc = G*S_true for the S channel (amplified) and A_adc = A_true/atten,
 * B_adc = B_true/atten for A/B (attenuated). Substituting into
 * S_true = x*A_true + (1-x)*B_true and solving for x in terms of the
 * ADC-domain quantities:
 *   S_adc/(G*atten) = x*A_adc + (1-x)*B_adc
 *   x = (S_adc/(G*atten) - B_adc) / (A_adc - B_adc) = (S_adc/k - B_adc)/(A_adc-B_adc)
 * with k = atten*G. Every channel is demodulated by the identical
 * per-sample transform (per-position sums combined by
 * math_phasor_combine(), same MATH_PHASOR_SAMPLES_PER_CYCLE/Q14 scale for
 * all four), so that shared digital scale factor cancels in this ratio --
 * the raw int64 I/Q sums can be used directly (just cast to float), no
 * separate normalization step needed. */

#define RING_MASK  (DISPLACEMENT_RING_DEPTH - 1U)
#if (DISPLACEMENT_RING_DEPTH & RING_MASK) != 0
#error "DISPLACEMENT_RING_DEPTH must be a power of two"
#endif

/* ~99% of ADS131M04_CODE_MAX (drv_ads131m04.h) -- "riding the rail"
 * margin so a sample doesn't have to hit the EXACT digital max/min to
 * count as clipped (real analog front-end clipping settles near, not
 * necessarily exactly at, the ADC's own rail). Checked per raw sample,
 * every channel, in on_sample() -- added 2026-09-26 alongside the S1/S2
 * PGA=16 bump, since that's exactly the change that could newly cause
 * this. */
#define ADS131M04_CLIP_THRESHOLD  ((int32_t)(ADS131M04_CODE_MAX / 100L * 99L))

static inline bool code_is_clipped(int32_t code)
{
    return code >= ADS131M04_CLIP_THRESHOLD || code <= -ADS131M04_CLIP_THRESHOLD;
}

/* One completed batch as the sample callback hands it to the task
 * (2026-10-03): for each of the 4 ADC channels (index = ADC channel, not the
 * signal: 0=S2, 1=B, 2=A, 3=S1), the plain int32 sum of all samples that
 * fell on each of the 8 positions of the carrier cycle over the batch's
 * DISPLACEMENT_BATCH_CYCLES cycles (config.h's "Per-position batch
 * accumulation" comment has the derivation and the range proof). The DFT
 * weights are applied later, once per batch, by math_phasor_combine().
 * seq = the per-cycle sequence number of the batch's LAST cycle. */
typedef struct {
    int32_t  pos_sum[4][MATH_PHASOR_SAMPLES_PER_CYCLE];
    uint16_t seq;
} RawBatch;

#define BATCH_RING_MASK  (DISPLACEMENT_BATCH_RING_DEPTH - 1U)
#if (DISPLACEMENT_BATCH_RING_DEPTH & BATCH_RING_MASK) != 0
#error "DISPLACEMENT_BATCH_RING_DEPTH must be a power of two"
#endif

/* A position sum adds DISPLACEMENT_BATCH_CYCLES signed 24-bit codes
 * (|code| <= 2^23), so |sum| <= BATCH_CYCLES * 2^23 must fit int32. */
_Static_assert(DISPLACEMENT_BATCH_CYCLES <= MATH_PHASOR_POS_SUM_MAX_CYCLES,
               "DISPLACEMENT_BATCH_CYCLES too large for int32 per-position sums");
_Static_assert(DISPLACEMENT_BATCH_CYCLES >= 1U && DISPLACEMENT_BATCH_CYCLES <= 255U,
               "DISPLACEMENT_BATCH_CYCLES must fit the uint8_t cycle counter");

/* Producer (sample callback) -> consumer (task) handoff, one entry per
 * completed BATCH -- lock-free SPSC, power-of-two size, drop-new-on-full
 * (CLAUDE.md 8.3). head is producer-owned (only on_sample() writes it),
 * tail is task-owned (only svc_displacement_update() writes it); each side
 * only reads the other's index, so no locking is needed beyond the
 * volatile qualifier. */
static RawBatch          s_in_ring[DISPLACEMENT_BATCH_RING_DEPTH];
static volatile uint16_t s_in_head = 0;
static volatile uint16_t s_in_tail = 0;

/* Task (producer) -> svc_displacement_pop() caller (consumer) handoff --
 * same SPSC/power-of-two/drop-new-on-full shape, one entry per computed
 * result. This is the per-cycle delta stream a future high-rate
 * consumer would drain -- nothing does yet on this REV B port's first
 * pass (see svc_displacement.h). */
static DisplacementCycle s_out_ring[DISPLACEMENT_RING_DEPTH];
static volatile uint16_t s_out_head = 0;
static volatile uint16_t s_out_tail = 0;

/* Sample-callback-only accumulators -- touched only from on_sample(),
 * always called from the same context (drv_ads131m04.c's SysTick frame
 * drain), so no volatile/locking needed here (same reasoning as WP8's
 * svc_signal_analysis.c). */
static uint8_t  s_sample_idx   = 0;                 /* position within the cycle, 0..7 */
static uint8_t  s_batch_cycles = 0;                 /* cycles completed in the batch in progress */
static int32_t  s_pos_sum[4][MATH_PHASOR_SAMPLES_PER_CYCLE];   /* [ADC channel][position] */

/* s_input_drop_count is written from on_sample() and read from
 * svc_displacement_get_input_drop_count() (task context) -- volatile,
 * same reasoning as s_in_head/s_in_tail above. s_output_drop_count and
 * s_degenerate_count are both written AND read only from task context,
 * so they don't need it. */
static volatile uint16_t s_input_drop_count = 0;
static uint16_t s_output_drop_count = 0;
static uint16_t s_degenerate_count  = 0;

/* s_clip_count is written from on_sample() (ISR-adjacent) -- volatile,
 * same reasoning as s_input_drop_count above. s_amplitude_fault_count is
 * written only from process_one_batch() (task context). See
 * svc_displacement.h's comment on the getters for what each actually
 * detects -- they are NOT two versions of the same check. */
static volatile uint16_t s_clip_count            = 0;
static uint16_t          s_amplitude_fault_count = 0;

/* Written/read only from task context (svc_displacement_start()/
 * svc_displacement_check_integrity()) -- no volatile needed. */
static bool s_fault_reported          = false;
static bool s_clip_logged             = false;
static bool s_amplitude_fault_logged  = false;

/* --- Bulk raw-ADC capture buffer (restored 2026-09-25, see
 * svc_displacement.h's comment) --- packed 24-bit codes, little-endian
 * signed, ch0..ch3 interleaved: 12 bytes per sample. Filled from
 * on_sample() (the same SysTick-frame-drain context, not a raw ISR) while
 * s_cap_active; drained by svc_api's bulk pump (task) once s_cap_done. */
static uint8_t           s_cap_buf[ADC_BULK_SAMPLE_COUNT * ADC_BULK_BYTES_PER_SAMPLE];
static volatile uint16_t s_cap_idx    = 0;
static volatile bool     s_cap_active = false;
static volatile bool     s_cap_done   = false;
static volatile uint32_t s_cap_t0     = 0;   /* tick at capture_begin() */
static volatile uint32_t s_cap_t1     = 0;   /* tick when the buffer filled */

/* Stats of the most recently completed capture — held past capture_end()
 * so a host diagnostic can read them back (effective sample rate =
 * samples / elapsed_ms). s_last_drops is the acquisition ring-overflow
 * count (drain fell a whole ring behind) accumulated during the fill —
 * the only lost-sample mechanism a full-rate capture has. */
static uint16_t s_last_samples    = 0;
static uint16_t s_last_drops      = 0;
static uint32_t s_last_elapsed_ms = 0;

/* Latest-batch snapshot for the API v2 Measurements (0x4) GET/SUBSCRIBE
 * resources (Services/svc_api.c) -- kept here, not in g_system_state,
 * same pattern as svc_battery_get_vbat_mv()/svc_powertest_mask() in
 * this codebase: a subsystem that owns values nothing but the API layer
 * reads exposes them via a getter instead of a shared-struct field.
 * (An early investigation into a real 2026-09-24 hang suspected these
 * writes specifically -- they were moved out of g_system_state while
 * chasing it. The actual root cause turned out to be unrelated
 * (config.h's ROOT-CAUSED comment, DISPLACEMENT_MAX_BATCHES_PER_TICK, has the full story, a
 * livelock, not a memory bug) and g_system_state would very likely have
 * been fine -- but the getter pattern is a reasonable fit regardless,
 * so it stayed.) Task context only (process_one_batch(), same context
 * as everything else in this file except on_sample()). */
static float s_delta1_mm  = 0.0f;
static float s_residual1  = 0.0f;
static float s_delta2_mm  = 0.0f;
static float s_residual2  = 0.0f;
static bool  s_disp_ok    = false;

/* Pre-moving-average delta_mm (2026-09-26), same storage/context/getter
 * rationale as s_delta1_mm above, kept separately so the raw ~40.7 Hz
 * batch stream stays available (Services/svc_api.c's Topic groups (0x5)
 * API2_RES_TOPIC_RAW_DISPLACEMENT) even though the Measurements
 * resources and the LIVE screen consume the post-MA s_delta1/2_mm. */
static float s_delta1_mm_raw = 0.0f;
static float s_delta2_mm_raw = 0.0f;

/* --- Post-division moving average (2026-09-26, config.h's
 * DISPLACEMENT_MA_SAMPLES comment has the full rationale) --- a boxcar
 * over the last DISPLACEMENT_MA_SAMPLES batches' delta_mm, applied here
 * in process_one_batch() before s_delta1_mm/s_delta2_mm are updated, so
 * every consumer of the getters below (the LIVE screen, API Measurements)
 * sees the smoothed value transparently. Task context only, same as
 * everything else in this block. */
static float   s_ma1_buf[DISPLACEMENT_MA_SAMPLES];
static float   s_ma2_buf[DISPLACEMENT_MA_SAMPLES];
static float   s_ma1_sum  = 0.0f;
static float   s_ma2_sum  = 0.0f;
static uint8_t s_ma_idx   = 0;
static uint8_t s_ma_count = 0;   /* ramps 0..DISPLACEMENT_MA_SAMPLES during
                                    * warm-up so the first few batches after
                                    * a start aren't biased toward zero by
                                    * an empty window */

/* Latest completed batch's raw phasors -- same storage/context reasoning
 * as the block above, added 2026-09-25 for the API v2 Topic groups (0x5)
 * real-time diagnostic resource. Written alongside s_delta1_mm etc. in
 * process_one_batch(). */
static DisplacementPhasors s_phasors = {0};

/* --- Bulk phasor log capture state (2026-09-25) --- see
 * svc_displacement.h's comment. Unlike the raw-ADC capture's s_cap_*
 * flags above (which on_sample(), the ISR-adjacent producer, touches
 * directly), all of this is written and read only from task context --
 * svc_displacement_update()'s batch-complete branch (the "producer" here)
 * and svc_api.c's bulk dispatch (the "consumer") are both called from
 * App/app_scheduler.c task functions, never from on_sample() -- so no
 * volatile is needed. */
static DisplacementPhasorLogEntry s_phasor_log[DISPLACEMENT_PHASOR_LOG_DEPTH];
static uint16_t s_phasor_log_idx         = 0;
static bool     s_phasor_log_active      = false;
static bool     s_phasor_log_done        = false;
static uint8_t  s_phasor_log_decim_count = 0;

/* Continuous phasor batch stream: FIFO filled by svc_displacement_update(),
 * drained by the API layer (svc_displacement.h "Continuous phasor batch
 * stream"). Both sides run in task context, so no locking is needed. */
#define PSTREAM_MASK (DISPLACEMENT_PHASOR_STREAM_DEPTH - 1U)
#if (DISPLACEMENT_PHASOR_STREAM_DEPTH & PSTREAM_MASK) != 0
#error "DISPLACEMENT_PHASOR_STREAM_DEPTH must be a power of two"
#endif
static DisplacementPhasorLogEntry s_pstream[DISPLACEMENT_PHASOR_STREAM_DEPTH];
static uint16_t s_pstream_head   = 0;
static uint16_t s_pstream_tail   = 0;
static uint16_t s_pstream_drops  = 0;
static bool     s_pstream_active = false;

/* --- Zero calibration state (2026-09-25) --- see svc_displacement.h's
 * comment. Task context only, same reasoning as the phasor log state
 * above (the "producer" is process_one_batch(), the "consumer" is
 * svc_api.c's svc_api_update(), both task-context callers). */
static DisplacementZeroCalPhase s_zero_cal_phase = DISP_ZERO_CAL_IDLE;
static uint16_t s_zero_cal_count = 0;
static float    s_zero_cal_sum1  = 0.0f;   /* running sum for the CURRENT step */
static float    s_zero_cal_sum2  = 0.0f;
static float    s_zero_cal_step1_avg1 = 0.0f;   /* saved once step 1 completes */
static float    s_zero_cal_step1_avg2 = 0.0f;
static float    s_zero_cal_result1_mm = 0.0f;   /* new absolute zero_offset, once RESULT_READY */
static float    s_zero_cal_result2_mm = 0.0f;

/* --- Per-batch quality flag (2026-09-26) --- see svc_displacement.h's
 * getter comment. Task context only, same reasoning as the zero-cal state
 * above. One EWMA baseline + previous-residual value per sensor, plus the
 * latest batch's pass/fail verdict. */
static float s_quality1_prev_residual = 0.0f;
static float s_quality2_prev_residual = 0.0f;
static float s_quality1_baseline      = 0.0f;   /* EWMA of |residual step|, good batches only */
static float s_quality2_baseline      = 0.0f;
static bool  s_quality1_seeded        = false;   /* first batch after start() seeds rather than EWMAs */
static bool  s_quality2_seeded        = false;
static bool  s_quality1_ok            = true;
static bool  s_quality2_ok            = true;

/* --- Triggered precision measurement state (2026-09-26) --- see
 * svc_displacement.h's comment. Task context only, same reasoning as the
 * zero-cal state above (the "producer" is process_one_batch(), the
 * "consumer" is Services/svc_api.c's command handler). sum1/2 are double,
 * not float: summing up to DISPLACEMENT_PRECISION_TARGET_SAMPLES mm-scale
 * floats is cheap either way at this rate (~20 Hz), so there's no reason
 * to accept float's coarser accumulation precision for a result whose
 * whole purpose is being the "reliable" one. */
static DisplacementPrecisionPhase s_precision_phase     = DISP_PRECISION_IDLE;
static uint16_t s_precision_count1    = 0;
static uint16_t s_precision_count2    = 0;
static double   s_precision_sum1      = 0.0;
static double   s_precision_sum2      = 0.0;
static float    s_precision_result1_mm = 0.0f;
static float    s_precision_result2_mm = 0.0f;
/* Differential (S1-S2) accumulator (2026-09-27) -- separate from sum1/sum2
 * above because it must average delta1[i]-delta2[i] for the SAME batch i,
 * not sum1/count1 - sum2/count2 (which would average over two potentially
 * different sets of batches, losing the point of excluding jointly). See
 * svc_displacement.h's precision-measurement comment. */
static uint16_t s_precision_count_diff     = 0;
static double   s_precision_sum_diff       = 0.0;
static float    s_precision_result_diff_mm = 0.0f;
static uint32_t s_precision_start_ms  = 0;
static bool     s_precision_timed_out = false;

/* --- Scheduler-gap diagnostic (2026-09-26) --- added specifically to
 * investigate why observed batch throughput and input_drop_count run
 * worse than DISPLACEMENT_BATCH_CYCLES/production-rate math alone
 * predicts. Tracks the longest gap between consecutive
 * svc_displacement_update() calls -- since the driver-level acquisition
 * (drv_ads131m04's own frame_deficit/ring_overflow) has repeatedly
 * measured perfectly healthy while svc_displacement's OWN input ring
 * still drops, the drops must come from this task not being re-entered
 * promptly by the scheduler, not from the ADC/DMA layer -- this
 * quantifies exactly how large that gap gets and, via
 * s_max_gap_at_uptime_ms, roughly when, so it can be correlated against
 * known periodic costs (the LIVE screen's ~2 s redraw cycle, BME280's
 * ~1 s poll, etc). Saturating like the other counters; reset at
 * start(). */
static uint32_t s_last_update_call_ms   = 0;
static bool     s_last_update_call_set  = false;
static uint16_t s_max_update_gap_ms     = 0;
static uint32_t s_max_gap_at_uptime_ms  = 0;

/* Frequency companion to the max above -- see config.h's
 * DISPLACEMENT_GAP_WARN_THRESHOLD_MS comment. Saturating like the other
 * counters; reset at start(). */
static uint16_t s_gap_over_threshold_count = 0;

/* Same reasoning as s_sample_idx above -- assigned in on_sample() to
 * every completed 8-sample cycle *before* the input-ring-full check, so
 * a batch dropped there (consumer not keeping up) still consumes its
 * DISPLACEMENT_BATCH_CYCLES seq values and shows up as a gap downstream.
 *
 * Rolls over at 65536 cycles (~25 s at ~2.6 kHz) -- a future stream
 * consumer doing gap detection MUST compare seq values with
 * wraparound-safe (modular) arithmetic, e.g. `(uint16_t)(seq -
 * expected) != 0`, not a naive `seq != prev + 1` or `seq < prev`. */
static uint16_t s_cycle_seq = 0;

/* Clears the sample callback's accumulation state and both input-side
 * indices. Task context, and only while the driver's trigger is NOT armed
 * (on_sample() runs only then) -- same reasoning as svc_displacement_start()'s
 * comment. s_cycle_seq restarts too so a fresh run's seq starts at
 * DISPLACEMENT_BATCH_CYCLES-1 for the first batch. */
static void accum_reset(void)
{
    s_sample_idx   = 0;
    s_batch_cycles = 0;
    s_cycle_seq    = 0;
    for (uint8_t ch = 0; ch < 4U; ++ch) {
        for (uint8_t n = 0; n < MATH_PHASOR_SAMPLES_PER_CYCLE; ++n) {
            s_pos_sum[ch][n] = 0;
        }
    }
    s_in_head = s_in_tail = 0;
}

static void ma_reset(void)
{
    for (uint8_t i = 0; i < DISPLACEMENT_MA_SAMPLES; ++i) {
        s_ma1_buf[i] = 0.0f;
        s_ma2_buf[i] = 0.0f;
    }
    s_ma1_sum = s_ma2_sum = 0.0f;
    s_ma_idx = s_ma_count = 0;
}

static void quality_reset(void)
{
    s_quality1_prev_residual = s_quality2_prev_residual = 0.0f;
    s_quality1_baseline      = s_quality2_baseline      = 0.0f;
    s_quality1_seeded        = s_quality2_seeded        = false;
    s_quality1_ok            = s_quality2_ok            = true;
}

/* Feeds one batch's raw (pre-MA) delta_mm into the moving-average window
 * for both sensors and returns the smoothed values. Must be called
 * exactly once per batch -- advances the shared window position and
 * warm-up count. */
static void ma_apply(float delta1, float delta2, float *out1, float *out2)
{
    s_ma1_sum += delta1 - s_ma1_buf[s_ma_idx];
    s_ma1_buf[s_ma_idx] = delta1;
    s_ma2_sum += delta2 - s_ma2_buf[s_ma_idx];
    s_ma2_buf[s_ma_idx] = delta2;

    if (s_ma_count < DISPLACEMENT_MA_SAMPLES) {
        s_ma_count++;
    }
    s_ma_idx = (uint8_t)((s_ma_idx + 1U) % DISPLACEMENT_MA_SAMPLES);

    *out1 = s_ma1_sum / (float)s_ma_count;
    *out2 = s_ma2_sum / (float)s_ma_count;
}

static void note_saturating(volatile uint16_t *counter)
{
    if (*counter < UINT16_MAX) {
        (*counter)++;
    }
}

static void on_sample(int32_t ch0, int32_t ch1, int32_t ch2, int32_t ch3)
{
    /* Bulk-capture mode: just store the raw codes and get out. The phasor
     * accumulation below is skipped so this stays trivially cheap for the
     * ~0.3 s a capture runs (see svc_displacement.h). Mutually exclusive
     * with the demod path by construction -- svc_api's bulk dispatch only
     * arms this while svc_displacement_is_running() is false. */
    if (s_cap_active) {
        if (s_cap_idx < ADC_BULK_SAMPLE_COUNT) {
            uint8_t *p = &s_cap_buf[(size_t)s_cap_idx * ADC_BULK_BYTES_PER_SAMPLE];
            const int32_t v[4] = { ch0, ch1, ch2, ch3 };
            for (uint8_t i = 0; i < 4U; ++i) {
                p[i * 3 + 0] = (uint8_t)(v[i] & 0xFF);
                p[i * 3 + 1] = (uint8_t)((v[i] >> 8) & 0xFF);
                p[i * 3 + 2] = (uint8_t)((v[i] >> 16) & 0xFF);
            }
            if (++s_cap_idx >= ADC_BULK_SAMPLE_COUNT) {
                s_cap_t1   = hal_systick_get_ms();
                s_cap_done = true;
            }
        }
        return;
    }

    /* Raw-code clip check, every sample, every channel -- see this file's
     * ADS131M04_CLIP_THRESHOLD comment. Cheap (4 compares), so it's fine
     * in this hot path unconditionally. */
    if (code_is_clipped(ch0) || code_is_clipped(ch1)
        || code_is_clipped(ch2) || code_is_clipped(ch3)) {
        note_saturating(&s_clip_count);
    }

    /* One plain 32-bit add per channel per sample into the sum for this
     * sample's position within the 8-sample carrier cycle (ch0=S2, ch1=B,
     * ch2=A, ch3=S1 -- see this file's top comment). No multiply, no sign
     * logic, no 64-bit math: the DFT weights are applied once per batch by
     * math_phasor_combine() (config.h's "Per-position batch accumulation"
     * comment). Cannot overflow: DISPLACEMENT_BATCH_CYCLES signed 24-bit codes
     * per position (static-asserted above). */
    s_pos_sum[0][s_sample_idx] += ch0;
    s_pos_sum[1][s_sample_idx] += ch1;
    s_pos_sum[2][s_sample_idx] += ch2;
    s_pos_sum[3][s_sample_idx] += ch3;

    s_sample_idx++;
    if (s_sample_idx < MATH_PHASOR_SAMPLES_PER_CYCLE) {
        return;
    }
    s_sample_idx = 0;
    uint16_t seq = s_cycle_seq++;

    if (++s_batch_cycles < DISPLACEMENT_BATCH_CYCLES) {
        return;
    }
    s_batch_cycles = 0;

    /* Batch complete: hand the 32 sums to the task. */
    uint16_t head = s_in_head;
    uint16_t next = (uint16_t)((head + 1U) & BATCH_RING_MASK);
    if (next == s_in_tail) {
        /* Consumer isn't keeping up -- drop this whole batch rather than
         * overwrite one it hasn't read yet. DELIBERATELY the opposite
         * policy from push_output() below: s_in_tail is task-owned (only
         * svc_displacement_update() writes it), so evicting it from here
         * would violate this ring's single-writer invariant for the tail
         * index. Counted in cycles (the unit this counter always had); the
         * seq counter already advanced for these cycles, so downstream sees
         * a seq jump of DISPLACEMENT_BATCH_CYCLES per dropped batch. */
        uint32_t dropped = (uint32_t)s_input_drop_count + DISPLACEMENT_BATCH_CYCLES;
        s_input_drop_count = (dropped > UINT16_MAX) ? UINT16_MAX : (uint16_t)dropped;
    } else {
        RawBatch *slot = &s_in_ring[head];
        for (uint8_t ch = 0; ch < 4U; ++ch) {
            for (uint8_t n = 0; n < MATH_PHASOR_SAMPLES_PER_CYCLE; ++n) {
                slot->pos_sum[ch][n] = s_pos_sum[ch][n];
            }
        }
        slot->seq = seq;
        s_in_head = next;
    }

    for (uint8_t ch = 0; ch < 4U; ++ch) {
        for (uint8_t n = 0; n < MATH_PHASOR_SAMPLES_PER_CYCLE; ++n) {
            s_pos_sum[ch][n] = 0;
        }
    }
}

static void push_output(uint16_t seq, float delta1, float residual1, float delta2, float residual2)
{
    uint16_t head = s_out_head;
    uint16_t next = (uint16_t)((head + 1U) & RING_MASK);
    if (next == s_out_tail) {
        /* Full -- overwrite the oldest unread entry rather than drop the
         * newest. Deliberately the opposite policy from the input ring
         * above: whether or not a consumer is currently draining this,
         * drop-new would mean the buffer permanently freezes at whatever
         * cycles happened to be produced while nobody was reading.
         * Overwrite-oldest keeps this always holding the most recent
         * results instead -- now DISPLACEMENT_BATCH_CYCLES batches deep
         * rather than raw cycles, since svc_displacement_update() only
         * calls process_one_batch() (and therefore this) once per batch. */
        s_out_tail = (uint16_t)((s_out_tail + 1U) & RING_MASK);
        note_saturating(&s_output_drop_count);
    }
    s_out_ring[head].delta1_mm = delta1;
    s_out_ring[head].residual1 = residual1;
    s_out_ring[head].delta2_mm = delta2;
    s_out_ring[head].residual2 = residual2;
    s_out_ring[head].seq       = seq;
    s_out_head = next;
}

/* Per-sensor calibration, converted from DeviceSettings' EEPROM-backed
 * scaled integers (milli-units / micrometers -- see system_state.h) to
 * float once per cycle. Scaled integers, not raw floats, on the
 * settings side: svc_api.c's SF() field machinery (Services/svc_api.c)
 * is integer-only, matching every other calibration constant in this
 * codebase -- see config.h's DEFAULT_DISP_* comment. */
typedef struct {
    float gain;              /* S-channel amplifier gain */
    float d0_mm;              /* effective scale factor, mm -- see load_sensor_cal() */
    float zero_offset_mm;    /* displacement zero calibration, mm */
} SensorCalF;

/* d0 (2026-09-27) is now the PRODUCT of a theoretical baseline and a
 * calibration factor, not one directly-settable number -- see config.h's
 * "Displacement sensitivity: theoretical baseline" comment for the full
 * Wyler-handbook derivation. d0_theoretical_um is what the Wyler spec's
 * 20uV RMS = 1um/m implies for THIS batch's actual x-sensitivity.
 *
 * sensitivity_uv_per_um_milli (2026-09-29, replacing the original bare
 * "cal_mult" ratio -- see system_state.h's field comment for why a
 * dimensionless multiplier on an assumption buried in firmware was
 * confusing in a way a direct physical quantity isn't) is this sensor's
 * REAL measured electrical sensitivity, in the SAME uV-per-0.001mm/m units
 * as the fixed nominal spec (DISPLACEMENT_WYLER_UV_RMS_PER_UM_PER_M,
 * config.h, =20). cal_mult is now just an internal implementation detail
 * derived from it here (nominal/actual -- if this sensor produces FEWER uV
 * per unit tilt than the 20uV spec claims, cal_mult > 1, compensating by
 * exactly that shortfall), not something stored or exposed on its own.
 *
 * zero_offset_um (2026-09-29, root-caused after a cal_mult change left two
 * instruments reading several mm at true level): stored in the SAME
 * cal_mult-independent theoretical domain as d0_theoretical -- i.e. "what
 * this sensor's electrical zero error would read at the nominal spec's
 * sensitivity" -- not the final output-mm domain the field's own name
 * might suggest. The sensor's intrinsic zero error lives in x_re itself (a
 * fixed offset from 0.5, baked in before ANY d0/cal_mult scaling is
 * applied), so it has to be scaled by the SAME d0_theoretical*cal_mult
 * factor as the real signal every time it's used, not stored once as a
 * fixed output-mm constant -- subtracting a fixed mm value was exactly
 * what broke every time cal_mult changed (the zero_offset_mm
 * docs/wp10_displacement.md's "should be re-run" note flagged as a
 * known-but-unfixed hazard, then hit repeatedly in practice). Multiplying
 * by the derived cal_mult here, at read time, makes a SINGLE zero-cal run
 * permanently valid across any future sensitivity change -- no more "redo
 * zero-cal after every gain adjustment." */
/* nominal/actual -- see svc_displacement.h's declaration comment. Shared
 * by load_sensor_cal() below, zero_cal_accumulate()'s step2-combination
 * math, and svc_api.c's zero_cal_apply_if_ready(), so none of them can
 * drift apart on how a sensitivity value becomes a cal_mult. */
float svc_displacement_cal_mult_from_sensitivity(int32_t sensitivity_uv_per_um_milli)
{
    float sensitivity = (float)sensitivity_uv_per_um_milli / 1000.0f;   /* uV per 0.001mm/m */
    return (float)DISPLACEMENT_WYLER_UV_RMS_PER_UM_PER_M / sensitivity;
}

static void load_sensor_cal(SensorCalF *out, int32_t gain_milli,
                             int32_t d0_theoretical_um, int32_t sensitivity_uv_per_um_milli,
                             int32_t zero_offset_um)
{
    float cal_mult = svc_displacement_cal_mult_from_sensitivity(sensitivity_uv_per_um_milli);
    out->gain           = (float)gain_milli / 1000.0f;
    out->d0_mm          = ((float)d0_theoretical_um / 1000.0f) * cal_mult;
    out->zero_offset_mm = ((float)zero_offset_um / 1000.0f) * cal_mult;
}

/* The values every sensor's computation needs but that don't vary
 * between sensors within one batch -- bundled so process_one_batch()'s
 * two compute_sensor_delta() calls below can't have same-typed adjacent
 * float arguments transposed by a future edit to one call site but not
 * the other.
 *
 * inv_den_re/inv_den_im is 1/(A-B), already inverted -- both sensors
 * divide by the identical (A-B) denominator, so process_one_batch()
 * computes that one reciprocal once via math_complex_reciprocal() and
 * both calls below multiply by it instead of each dividing separately
 * (division is the most expensive op available on this FPU-less
 * Cortex-M0+; multiplication is much cheaper). */
typedef struct {
    float atten;
    float inv_den_re, inv_den_im;   /* 1 / (A - B) */
} SharedCycleTerms;

/* One sensor's x/delta/residual, given the shared (A-B) reciprocal --
 * factored out so process_one_batch() below computes S1 and S2 the same
 * way instead of two hand-duplicated copies. Cannot fail -- the only
 * degenerate case (A-B exactly zero) is checked once in
 * process_one_batch() before this is called, since it's identical for
 * both sensors.
 *
 * The B subtraction dropped 2026-09-30 (docs/signal_processing.tex
 * Section 12, "drop the B term") -- x used to be (S/k - B)/(A-B), which
 * expands to S/(k(A-B)) + z, z = -(A+B)/(2(A-B)) being an "imbalance
 * term" meant to auto-cancel a real A/B excitation imbalance AT THE
 * SENSOR, assuming the sensor's own output carries the excitation
 * midpoint. The 24h granite-plate run's natural charging-start heating
 * event showed the sensor does NOT carry that midpoint (S barely moved
 * while z jumped sharply) -- so z was never cancelling a real imbalance,
 * it was injecting A/B MEASUREMENT-PATH drift (temperature-dependent
 * channel mismatch) straight into the reading. Removing it cut S1's
 * drift ~4x and S2's ~8x on that dataset, with S1-S2 unchanged (z is
 * common-mode, already cancelled there). d0/sensitivity stay valid (the
 * tilt term x=S/(k(A-B)) is unchanged); zero-cal has to be redone since
 * the static Re(z) baseline was folded into the old zero offset. */
static void compute_sensor_delta(float iS, float qS, const SensorCalF *cal,
                                  const SharedCycleTerms *shared,
                                  float *delta_out, float *residual_out)
{
    /* S/k -- a real-scalar reciprocal-multiply (k = atten*gain has no
     * imaginary part), not a complex operation. */
    float inv_k = 1.0f / (shared->atten * cal->gain);
    float num_re = iS * inv_k;
    float num_im = qS * inv_k;

    /* x' = num * (1/den) -- complex multiply by the precomputed shared
     * reciprocal, equivalent to num/den but without a division here. */
    float x_re = num_re * shared->inv_den_re - num_im * shared->inv_den_im;
    float x_im = num_re * shared->inv_den_im + num_im * shared->inv_den_re;

    /* No "- 0.5" here (2026-09-30, alongside the B-term drop above): that
     * offset existed because the OLD x = (S/k-B)/(A-B) sits near 0.5 at
     * true zero tilt (B contributes a baseline the old formula had to
     * re-center). x' = S/(k(A-B)) has no such baseline -- it's naturally
     * near 0 at zero tilt (docs/signal_processing.tex Section 12.7's
     * "tilt = 2*d0*Re(x') - zero", no 1/2 term). Leaving "-0.5" in after
     * dropping B would subtract a spurious d0-sized constant from every
     * reading -- exactly the ~1.1-1.4m bogus offset this was caught by. */
    *delta_out    = 2.0f * cal->d0_mm * x_re - cal->zero_offset_mm;
    *residual_out = x_im;
}

/* DISPLACEMENT_BATCH_CYCLES consecutive cycles' raw I/Q, coherently
 * summed (config.h's ROOT-CAUSED comment on DISPLACEMENT_BATCH_CYCLES
 * has the full story on why this exists: single-cycle division-heavy
 * math couldn't sustain the 2.6 kHz production rate once its result was
 * stored anywhere). A plain int64 struct instead of loose same-typed
 * parameters, for the same reason SharedCycleTerms above is a struct --
 * eight adjacent int64_t arguments would be a silent-transposition
 * hazard on a future edit. */
typedef struct {
    int64_t iB, qB, iA, qA, iS1, qS1, iS2, qS2;
} BatchSums;

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
        s_zero_cal_result1_mm = (float)g_device_settings.disp_s1_zero_offset_um / 1000.0f
                               * svc_displacement_cal_mult_from_sensitivity(g_device_settings.disp_s1_sensitivity_uv_per_um_milli)
                               + (s_zero_cal_step1_avg1 + avg1) / 2.0f;
        s_zero_cal_result2_mm = (float)g_device_settings.disp_s2_zero_offset_um / 1000.0f
                               * svc_displacement_cal_mult_from_sensitivity(g_device_settings.disp_s2_sensitivity_uv_per_um_milli)
                               + (s_zero_cal_step1_avg2 + avg2) / 2.0f;
        s_zero_cal_phase = DISP_ZERO_CAL_RESULT_READY;
    }
}

/* One sensor's quality check for the batch that just completed -- see
 * config.h's DISPLACEMENT_QUALITY_BAD_MULTIPLE comment for the bench
 * validation behind this. *prev_residual and *baseline are this specific
 * sensor's persistent state (the caller passes S1's or S2's, never mixed).
 * The very first call after a start() seeds the baseline directly from
 * that first step rather than EWMA-ing into a zero-initialized baseline
 * (which would flag nearly everything as bad until the EWMA warmed up) --
 * *seeded tracks whether that's already happened. Returns true (good) on
 * that seeding call, since there's nothing yet to judge it against. */
static bool quality_update(float residual, float *prev_residual, float *baseline, bool *seeded)
{
    float step = residual - *prev_residual;
    float astep = (step < 0.0f) ? -step : step;
    *prev_residual = residual;

    if (!*seeded) {
        *baseline = astep;
        *seeded = true;
        return true;
    }

    bool good = astep <= ((float)DISPLACEMENT_QUALITY_BAD_MULTIPLE * (*baseline));
    if (good) {
        /* EWMA over good batches only -- a sustained noisy patch must not
         * be allowed to inflate the baseline and raise its own bar (see
         * config.h's comment). */
        *baseline += (astep - *baseline) / (float)DISPLACEMENT_QUALITY_EWMA_SAMPLES;
    }
    return good;
}

/* Finishes the current precision-measurement run (target reached on both
 * sensors, or the timeout fired) -- computes the mean of whatever was
 * actually collected per sensor (0 if a sensor never got a single good
 * batch, e.g. a pathological all-bad run) and moves to DONE. Shared by
 * precision_accumulate() (the target-reached path) and
 * svc_displacement_update()'s periodic timeout check below (the
 * timeout-fired path, needed because nothing else drives this state
 * forward if on_sample()/process_one_batch() stalls mid-run). */
static void precision_finish(bool timed_out)
{
    s_precision_timed_out = timed_out;
    s_precision_result1_mm = (s_precision_count1 > 0U)
        ? (float)(s_precision_sum1 / (double)s_precision_count1) : 0.0f;
    s_precision_result2_mm = (s_precision_count2 > 0U)
        ? (float)(s_precision_sum2 / (double)s_precision_count2) : 0.0f;
    s_precision_result_diff_mm = (s_precision_count_diff > 0U)
        ? (float)(s_precision_sum_diff / (double)s_precision_count_diff) : 0.0f;
    s_precision_phase = DISP_PRECISION_DONE;
}

/* Feeds one batch's raw delta_mm + quality verdict into an in-progress
 * precision-measurement run, if one is armed (cheap no-op via the phase
 * check when idle, same shape as zero_cal_accumulate() above). Each
 * sensor accumulates independently -- a channel with a worse quality-good
 * rate simply takes longer to reach the target, up to the shared timeout.
 * The differential (2026-09-27) accumulates delta1-delta2 for this SAME
 * batch, gated on BOTH good1 AND good2 -- "exclude the differential
 * reading if either input is bad" (svc_displacement.h's comment). */
static void precision_accumulate(float delta1, bool good1, float delta2, bool good2)
{
    if (s_precision_phase != DISP_PRECISION_RUNNING) {
        return;
    }
    if (good1 && s_precision_count1 < DISPLACEMENT_PRECISION_TARGET_SAMPLES) {
        s_precision_sum1 += (double)delta1;
        s_precision_count1++;
    }
    if (good2 && s_precision_count2 < DISPLACEMENT_PRECISION_TARGET_SAMPLES) {
        s_precision_sum2 += (double)delta2;
        s_precision_count2++;
    }
    if (good1 && good2 && s_precision_count_diff < DISPLACEMENT_PRECISION_TARGET_SAMPLES) {
        s_precision_sum_diff += (double)(delta1 - delta2);
        s_precision_count_diff++;
    }
    if (s_precision_count1 >= DISPLACEMENT_PRECISION_TARGET_SAMPLES
        && s_precision_count2 >= DISPLACEMENT_PRECISION_TARGET_SAMPLES
        && s_precision_count_diff >= DISPLACEMENT_PRECISION_TARGET_SAMPLES) {
        precision_finish(false);
    }
}

/* Highest |I+jQ| a genuinely full-scale (ADS131M04_CODE_MAX-amplitude),
 * UNDISTORTED sinusoid at exactly the matched carrier frequency could
 * ever produce over one DISPLACEMENT_BATCH_CYCLES-cycle batch.
 * Derivation: math_phasor.c's 8-point matched-filter sum, for
 * sample[n] = A*cos(n*45deg - phi), gives i_sum = 65536*A*cos(phi),
 * q_sum = 65536*A*sin(phi) over one cycle (the cross terms in the
 * product-to-sum expansion sum to zero over a full period) -- magnitude
 * 65536*A regardless of phi. A DISPLACEMENT_BATCH_CYCLES-cycle coherent
 * sum, worst case perfectly in phase throughout, is that many times
 * bigger. See svc_displacement.h's getter comment for what exceeding
 * this actually means (NOT clipping -- see there). double, not float:
 * this reaches ~1e14 at the default batch size, well past float's exact-
 * integer range, though only a coarse threshold comparison is needed
 * here, not precision. */
#define DISPLACEMENT_MAX_THEORETICAL_PHASOR_MAG \
    ((double)DISPLACEMENT_BATCH_CYCLES * 65536.0 * (double)ADS131M04_CODE_MAX)

static bool phasor_exceeds_theoretical_max(int64_t i_sum, int64_t q_sum)
{
    double i = (double)i_sum, q = (double)q_sum;
    double mag2     = i * i + q * q;
    double max_mag  = DISPLACEMENT_MAX_THEORETICAL_PHASOR_MAG;
    return mag2 > (max_mag * max_mag);
}

static void process_one_batch(const BatchSums *s, uint16_t seq)
{
    float iB = (float)s->iB, qB = (float)s->qB;
    float iA = (float)s->iA, qA = (float)s->qA;

    /* Stash the raw phasors before the degenerate-denominator check below
     * can return early -- a diagnostic host looking at WHY the excitation
     * phasors are degenerate needs this snapshot precisely when the delta
     * math itself can't produce one. */
    s_phasors.iB = iB;  s_phasors.qB = qB;
    s_phasors.iA = iA;  s_phasors.qA = qA;
    s_phasors.iS1 = (float)s->iS1;  s_phasors.qS1 = (float)s->qS1;
    s_phasors.iS2 = (float)s->iS2;  s_phasors.qS2 = (float)s->qS2;

    if (phasor_exceeds_theoretical_max(s->iB, s->qB)
        || phasor_exceeds_theoretical_max(s->iA, s->qA)
        || phasor_exceeds_theoretical_max(s->iS1, s->qS1)
        || phasor_exceeds_theoretical_max(s->iS2, s->qS2)) {
        note_saturating(&s_amplitude_fault_count);
    }

    /* 1/(A - B), the shared denominator's reciprocal -- computed once and
     * reused by both sensors below. */
    float inv_den_re, inv_den_im;
    if (!math_complex_reciprocal(iA - iB, qA - qB, &inv_den_re, &inv_den_im)) {
        /* A and B phasors exactly identical -- degenerate excitation,
         * shouldn't happen in practice. Skip rather than divide by
         * zero. Both sensors share this same (A-B) denominator, so if
         * it's degenerate it's degenerate for both -- checked once here
         * instead of once per sensor. */
        note_saturating(&s_degenerate_count);
        return;
    }

    SensorCalF s1_cal, s2_cal;
    load_sensor_cal(&s1_cal, g_device_settings.disp_s1_gain_milli,
                     g_device_settings.disp_s1_d0_theoretical_um, g_device_settings.disp_s1_sensitivity_uv_per_um_milli,
                     g_device_settings.disp_s1_zero_offset_um);
    load_sensor_cal(&s2_cal, g_device_settings.disp_s2_gain_milli,
                     g_device_settings.disp_s2_d0_theoretical_um, g_device_settings.disp_s2_sensitivity_uv_per_um_milli,
                     g_device_settings.disp_s2_zero_offset_um);

    SharedCycleTerms shared = {
        .atten      = (float)g_device_settings.disp_atten_milli / 1000.0f,
        .inv_den_re = inv_den_re,
        .inv_den_im = inv_den_im,
    };
    float delta1, residual1, delta2, residual2;
    compute_sensor_delta((float)s->iS1, (float)s->qS1, &s1_cal, &shared, &delta1, &residual1);
    compute_sensor_delta((float)s->iS2, (float)s->qS2, &s2_cal, &shared, &delta2, &residual2);

    push_output(seq, delta1, residual1, delta2, residual2);

    s_delta1_mm_raw = delta1;
    s_delta2_mm_raw = delta2;
    ma_apply(delta1, delta2, &s_delta1_mm, &s_delta2_mm);
    s_residual1 = residual1;
    s_residual2 = residual2;
    s_disp_ok   = true;

    s_quality1_ok = quality_update(residual1, &s_quality1_prev_residual,
                                    &s_quality1_baseline, &s_quality1_seeded);
    s_quality2_ok = quality_update(residual2, &s_quality2_prev_residual,
                                    &s_quality2_baseline, &s_quality2_seeded);

    zero_cal_accumulate(delta1, delta2);
    precision_accumulate(delta1, s_quality1_ok, delta2, s_quality2_ok);
}

DrvStatus svc_displacement_init(void)
{
    accum_reset();
    s_out_head = s_out_tail = 0;
    s_input_drop_count  = 0;
    s_output_drop_count = 0;
    s_degenerate_count  = 0;
    s_clip_count             = 0;
    s_amplitude_fault_count  = 0;
    s_clip_logged            = false;
    s_amplitude_fault_logged = false;
    s_disp_ok           = false;
    ma_reset();
    quality_reset();

    /* drv_ads131m04_init() resets its callback pointer to NULL as its
     * first action (Drivers_App/drv_ads131m04.c) -- must register AFTER
     * init(), not before, or init() silently wipes it and on_sample()
     * never runs again (bench-caught 2026-09-24: acquisition ran
     * perfectly -- frames_produced tracking frames_drained, zero faults
     * -- but disp_ok stayed false forever because nothing was ever
     * feeding the input ring). The never-merged wp10 branch this was
     * ported from called these in the opposite order; that driver
     * revision apparently didn't reset the callback in init(). */
    DrvStatus rc = drv_ads131m04_init();
    drv_ads131m04_set_on_sample(on_sample);
    return rc;
}

DrvStatus svc_displacement_start(void)
{
    /* Reset the rings and the sample-callback accumulators so the first
     * cycles after a start are clean, not a stale mix left over from a
     * previous run -- on_sample() only ever runs while the driver's
     * trigger is armed, so it's safe to touch its state here (task
     * context) before arming it. */
    accum_reset();
    s_out_head = s_out_tail = 0;
    s_fault_reported = false;
    s_clip_count             = 0;
    s_amplitude_fault_count  = 0;
    s_clip_logged            = false;
    s_amplitude_fault_logged = false;
    s_last_update_call_set   = false;
    s_max_update_gap_ms      = 0;
    s_max_gap_at_uptime_ms   = 0;
    s_gap_over_threshold_count = 0;
    s_disp_ok = false;
    ma_reset();
    quality_reset();
    /* A fresh start invalidates any in-progress zero-cal run -- its
     * averaging assumed a continuous demod session, not one straddling a
     * stop/start. Same for a precision measurement -- its averaging
     * assumed a continuous session too. */
    svc_displacement_zero_cal_cancel();
    svc_displacement_precision_cancel();
    return drv_ads131m04_start();
}

/* Sign convention is arbitrary at the sensor level -- disp_s1/s2_invert
 * (system_state.h) let each instrument's FINAL reported reading be flipped
 * so "front up" can read positive, without touching the gain/zero-cal math
 * upstream of it. Applied here, at the getter layer, deliberately -- every
 * external consumer (API Measurements/Topics, the LIVE screen, the
 * triggered precision measurement's result) reads through one of these
 * getters, so this is the one place that has to know about the flip.
 * zero_cal_accumulate()/precision_accumulate() upstream in
 * process_one_batch() use the UNFLIPPED delta1/delta2 locals directly, not
 * these getters -- zero-cal and the precision-run's live progress stay
 * entirely in the sensor's native sign convention; only the finished,
 * reported numbers flip. */
static float sensor_sign(uint8_t invert_flag) { return invert_flag ? -1.0f : 1.0f; }

float svc_displacement_get_delta1_mm(void) { return sensor_sign(g_device_settings.disp_s1_invert) * s_delta1_mm; }
float svc_displacement_get_residual1(void) { return s_residual1; }
float svc_displacement_get_delta2_mm(void) { return sensor_sign(g_device_settings.disp_s2_invert) * s_delta2_mm; }
float svc_displacement_get_residual2(void) { return s_residual2; }
bool  svc_displacement_get_ok(void)        { return s_disp_ok; }

float svc_displacement_get_delta1_mm_raw(void) { return sensor_sign(g_device_settings.disp_s1_invert) * s_delta1_mm_raw; }
float svc_displacement_get_delta2_mm_raw(void) { return sensor_sign(g_device_settings.disp_s2_invert) * s_delta2_mm_raw; }

/* Recomputed from the already-flipped individual getters above (not simply
 * `sign * (s_delta1_mm - s_delta2_mm)`), so this is correct even in the
 * unusual case where S1 and S2 are inverted differently from each other --
 * the two sensors are independent per system_state.h's comment, even
 * though in practice (both rigidly mounted in the same housing) they'll
 * almost always be set the same way. */
float svc_displacement_get_delta_diff_mm(void)
{
    return svc_displacement_get_delta1_mm() - svc_displacement_get_delta2_mm();
}
float svc_displacement_get_delta_diff_mm_raw(void)
{
    return svc_displacement_get_delta1_mm_raw() - svc_displacement_get_delta2_mm_raw();
}

bool svc_displacement_get_quality1_ok(void) { return s_quality1_ok; }
bool svc_displacement_get_quality2_ok(void) { return s_quality2_ok; }
bool svc_displacement_get_quality_diff_ok(void) { return s_quality1_ok && s_quality2_ok; }

void svc_displacement_get_phasors(DisplacementPhasors *out)
{
    if (out != 0) {
        *out = s_phasors;
    }
}

/* Peak amplitude (mV, referred to the ADC pin at this channel's own PGA)
 * from one raw phasor -- inverts math_phasor.c's per-cycle scale
 * (i_sum/q_sum = 65536*A_peak_code*cos/sin(phi) for one cycle, so a
 * DISPLACEMENT_BATCH_CYCLES-cycle coherent batch sum is that many times
 * bigger -- see config.h's DISPLACEMENT_MAX_THEORETICAL_PHASOR_MAG comment
 * for the same derivation) then the ADS131M04's own LSB size
 * (2.4V/PGA/ADS131M04_CODE_MAX, drv_ads131m04.h). */
static float phasor_peak_mv(float i, float q, uint8_t pga)
{
    float mag       = sqrtf(i * i + q * q);
    float peak_code = mag / (65536.0f * (float)DISPLACEMENT_BATCH_CYCLES);
    return peak_code * (2400.0f / (float)pga) / (float)ADS131M04_CODE_MAX;
}

void svc_displacement_get_signal_diag(DisplacementSignalDiag *out)
{
    if (out == 0) {
        return;
    }
    const float i[4] = { s_phasors.iB, s_phasors.iA, s_phasors.iS1, s_phasors.iS2 };
    const float q[4] = { s_phasors.qB, s_phasors.qA, s_phasors.qS1, s_phasors.qS2 };
    /* B, A at PGA=1; S1, S2 at PGA=16 -- must stay in sync with
     * Drivers_App/drv_ads131m04.c's GAIN1_REG_VALUE. */
    const uint8_t pga[4] = { 1U, 1U, 16U, 16U };

    for (uint8_t ch = 0; ch < 4U; ++ch) {
        float peak_mv    = phasor_peak_mv(i[ch], q[ch], pga[ch]);
        out->rms_mv[ch]   = peak_mv * 0.70710678f;   /* /sqrt(2) */
        out->p2p_mv[ch]   = peak_mv * 2.0f;
        out->phase_deg[ch] = atan2f(q[ch], i[ch]) * (180.0f / 3.14159265f);
    }

    /* Wyler-handbook-only estimate -- see svc_displacement.h's comment.
     * 20uV RMS = 1um/m => tilt_mm_per_m = S_rms_uV/20/1000 = S_rms_mV/20
     * (the uV->mV and um/m->mm/m factors of 1000 cancel exactly). */
    out->theoretical_tilt1_mm_per_m = out->rms_mv[2] / (float)DISPLACEMENT_WYLER_UV_RMS_PER_UM_PER_M;
    out->theoretical_tilt2_mm_per_m = out->rms_mv[3] / (float)DISPLACEMENT_WYLER_UV_RMS_PER_UM_PER_M;
}

void svc_displacement_stop(void)
{
    drv_ads131m04_stop();
    /* on_sample() only runs while the driver's trigger is armed (same
     * reasoning as svc_displacement_start()'s reset), so it's safe to
     * touch input-ring/batch state here in task context right after
     * disarming it. Without this, cycles already queued before the stop
     * get drained by the next task_displacement tick, complete a batch,
     * and re-set s_disp_ok = true right after the line below clears it. */
    accum_reset();
    /* disp_ok reports "have a currently-valid measurement", not just
     * "the last batch before stop succeeded" -- clear it so a host
     * reading Measurements 0x0D right after a stop doesn't see a stale
     * ok=1 from before acquisition was halted. */
    s_disp_ok = false;
    /* A stop mid-calibration leaves a stale, confusing in-progress state
     * (e.g. a step 1 average from before the stop, paired with a step 2
     * that never got to run) -- cancel rather than let a later
     * step2_begin() silently combine data from two different sessions.
     * Same reasoning for a precision measurement caught mid-run. */
    svc_displacement_zero_cal_cancel();
    svc_displacement_precision_cancel();
}

bool svc_displacement_is_running(void)
{
    return drv_ads131m04_is_running();
}

/* Stores one decimated snapshot for an active phasor-log capture (every
 * DISPLACEMENT_PHASOR_LOG_DECIMATIONth completed batch only) -- called
 * from svc_displacement_update() in place of process_one_batch() while
 * s_phasor_log_active. Task context only, same as everything else here
 * except on_sample(). */
static void store_phasor_log_entry(const BatchSums *s, uint16_t seq)
{
    if (++s_phasor_log_decim_count < DISPLACEMENT_PHASOR_LOG_DECIMATION) {
        return;
    }
    s_phasor_log_decim_count = 0;

    if (s_phasor_log_idx >= DISPLACEMENT_PHASOR_LOG_DEPTH) {
        return;   /* already full -- svc_displacement_phasor_log_done() is
                    * true and the caller will end() the capture shortly */
    }
    DisplacementPhasorLogEntry *e = &s_phasor_log[s_phasor_log_idx];
    e->iB  = (float)s->iB;  e->qB  = (float)s->qB;
    e->iA  = (float)s->iA;  e->qA  = (float)s->qA;
    e->iS1 = (float)s->iS1; e->qS1 = (float)s->qS1;
    e->iS2 = (float)s->iS2; e->qS2 = (float)s->qS2;
    e->seq = seq;

    if (++s_phasor_log_idx >= DISPLACEMENT_PHASOR_LOG_DEPTH) {
        s_phasor_log_done = true;
    }
}

/* Queues one completed batch for the continuous stream. A full FIFO drops
 * the NEWEST batch (counted) -- the consumer sees the loss as a seq jump. */
static void store_phasor_stream_entry(const BatchSums *s, uint16_t seq)
{
    uint16_t next = (uint16_t)((s_pstream_head + 1U) & PSTREAM_MASK);
    if (next == s_pstream_tail) {
        if (s_pstream_drops < UINT16_MAX) s_pstream_drops++;
        return;
    }
    DisplacementPhasorLogEntry *e = &s_pstream[s_pstream_head];
    e->iB  = (float)s->iB;  e->qB  = (float)s->qB;
    e->iA  = (float)s->iA;  e->qA  = (float)s->qA;
    e->iS1 = (float)s->iS1; e->qS1 = (float)s->qS1;
    e->iS2 = (float)s->iS2; e->qS2 = (float)s->qS2;
    e->seq = seq;
    s_pstream_head = next;
}

void svc_displacement_update(void)
{
    /* Scheduler-gap diagnostic -- see s_max_update_gap_ms's comment.
     * Measured before anything else in this function so the recorded gap
     * is purely "how long since the scheduler last reached this task",
     * not inflated by this call's own work. */
    uint32_t now_ms = hal_systick_get_ms();
    if (s_last_update_call_set) {
        uint32_t gap = now_ms - s_last_update_call_ms;
        if (gap > (uint32_t)s_max_update_gap_ms) {
            s_max_update_gap_ms    = (gap > 0xFFFFU) ? 0xFFFFU : (uint16_t)gap;
            s_max_gap_at_uptime_ms = now_ms;
        }
        if (gap >= (uint32_t)DISPLACEMENT_GAP_WARN_THRESHOLD_MS) {
            note_saturating(&s_gap_over_threshold_count);
        }
    }
    s_last_update_call_ms  = now_ms;
    s_last_update_call_set = true;

    /* Drain the completed batches queued since the last call. Each carries
     * the per-position int32 sums of one DISPLACEMENT_BATCH_CYCLES-cycle
     * batch (accumulated by on_sample(), one add per sample); the DFT
     * weights are applied here, once per batch, by math_phasor_combine(),
     * and process_one_batch()'s division-heavy math then runs once per batch
     * (config.h's DISPLACEMENT_BATCH_CYCLES comment has the full
     * root-caused reasoning). Bounded to at most
     * DISPLACEMENT_MAX_BATCHES_PER_TICK batches per call -- a real,
     * reproduced bug (2026-09-24, see config.h's ROOT-CAUSED comment) was
     * this loop running unbounded: if on_sample() ever queues work faster
     * than this can drain it, an unbounded `while (s_in_tail != s_in_head)`
     * never exits, and the scheduler's main loop never returns to run
     * anything else again -- confirmed with a debugger, not a HardFault, a
     * genuine livelock. This cap turns that failure mode into ordinary
     * graceful drops (s_input_drop_count, already handled) instead. */
    uint8_t drained = 0;
    while (s_in_tail != s_in_head && drained < DISPLACEMENT_MAX_BATCHES_PER_TICK) {
        const RawBatch *rb = &s_in_ring[s_in_tail];
        BatchSums sums;
        math_phasor_combine(rb->pos_sum[1], &sums.iB,  &sums.qB);    /* ch1 = B  */
        math_phasor_combine(rb->pos_sum[2], &sums.iA,  &sums.qA);    /* ch2 = A  */
        math_phasor_combine(rb->pos_sum[3], &sums.iS1, &sums.qS1);   /* ch3 = S1 */
        math_phasor_combine(rb->pos_sum[0], &sums.iS2, &sums.qS2);   /* ch0 = S2 */
        const uint16_t batch_seq = rb->seq;
        s_in_tail = (uint16_t)((s_in_tail + 1U) & BATCH_RING_MASK);
        drained++;

        /* A phasor-log capture (svc_displacement_phasor_log_begin(),
         * config.h's "Displacement phasor diagnostics" comment) wants the
         * raw batch sums stored, not demodulated -- same
         * accumulation/batching pipeline either way, this is the only
         * fork point. */
        if (s_pstream_active) {
            store_phasor_stream_entry(&sums, batch_seq);
        } else if (s_phasor_log_active) {
            store_phasor_log_entry(&sums, batch_seq);
        } else {
            process_one_batch(&sums, batch_seq);
        }
    }

    /* Precision-measurement timeout, checked every tick regardless of
     * whether a batch completed this pass -- precision_accumulate() above
     * only fires the "target reached" path, so this is what guarantees
     * the timeout still fires even if acquisition stalls or a channel's
     * quality-good rate is so poor it never reaches the target (see
     * config.h's DISPLACEMENT_PRECISION_TIMEOUT_MS comment). */
    if (s_precision_phase == DISP_PRECISION_RUNNING
        && (uint32_t)(hal_systick_get_ms() - s_precision_start_ms) >= DISPLACEMENT_PRECISION_TIMEOUT_MS) {
        precision_finish(true);
    }
}

void svc_displacement_check_integrity(void)
{
    /* Clip / amplitude-fault: logged once, edge-triggered (first
     * occurrence only) -- the counters themselves (API Raw data 0x7/0x02)
     * are the durable, ever-incrementing record; this is just the "look
     * at this" tripwire, not a per-occurrence log (clipping can recur at
     * up to the ~2.6 kHz sample rate, and this function runs every
     * scheduler tick -- logging every change would flood the debug-log
     * ring). Independent of the ADC-integrity fault check below: neither
     * stops acquisition, unlike that one. */
    if (!s_clip_logged && s_clip_count > 0U) {
        s_clip_logged = true;
        svc_logf(API2_LOG_WARN,
                 "displacement: ADC input clipping detected (count=%u)",
                 (unsigned)s_clip_count);
    }
    if (!s_amplitude_fault_logged && s_amplitude_fault_count > 0U) {
        s_amplitude_fault_logged = true;
        svc_logf(API2_LOG_ERROR,
                 "displacement: batch phasor exceeded theoretical max (count=%u) "
                 "-- check gain calibration / data integrity, not analog clipping",
                 (unsigned)s_amplitude_fault_count);
    }

    if (s_fault_reported || !drv_ads131m04_faulted()) {
        return;
    }
    s_fault_reported = true;

    const volatile Ads131m04Integrity *ig = drv_ads131m04_get_integrity();
    static const char *const names[] = { "none", "overrun", "framing", "crc", "slip" };
    uint8_t fc = ig->fault_code;
    svc_logf(API2_LOG_ERROR,
             "ADC integrity fault: %s (frames %lu ovf %lu framing %lu crc %lu "
             "deficit %ld [%ld,%ld] run %lu ms) — acquisition stopped",
             (fc < (sizeof names / sizeof names[0])) ? names[fc] : "?",
             (unsigned long)ig->frames_produced, (unsigned long)ig->ring_overflow,
             (unsigned long)ig->framing_err, (unsigned long)ig->crc_err,
             (long)ig->frame_deficit, (long)ig->frame_deficit_min,
             (long)ig->frame_deficit_max, (unsigned long)ig->run_ms);

    drv_ads131m04_stop();
}

bool svc_displacement_pop(DisplacementCycle *out)
{
    if (out == 0) {
        return false;
    }
    if (s_out_tail == s_out_head) {
        return false;   /* empty */
    }
    *out = s_out_ring[s_out_tail];
    s_out_tail = (uint16_t)((s_out_tail + 1U) & RING_MASK);
    return true;
}

uint16_t svc_displacement_get_input_drop_count(void)
{
    return s_input_drop_count;
}

uint16_t svc_displacement_get_output_drop_count(void)
{
    return s_output_drop_count;
}

uint16_t svc_displacement_get_degenerate_count(void)
{
    return s_degenerate_count;
}

uint16_t svc_displacement_get_clip_count(void)
{
    return s_clip_count;
}

void svc_displacement_get_max_update_gap(uint16_t *gap_ms_out, uint32_t *at_uptime_ms_out,
                                          uint16_t *over_threshold_count_out)
{
    if (gap_ms_out)             *gap_ms_out             = s_max_update_gap_ms;
    if (at_uptime_ms_out)       *at_uptime_ms_out        = s_max_gap_at_uptime_ms;
    if (over_threshold_count_out) *over_threshold_count_out = s_gap_over_threshold_count;
}

uint16_t svc_displacement_get_amplitude_fault_count(void)
{
    return s_amplitude_fault_count;
}

DrvStatus svc_displacement_capture_begin(void)
{
    if (s_cap_active) {
        return DRV_ERR_NOT_READY;
    }
    s_cap_idx    = 0;
    s_cap_done   = false;
    s_cap_t1     = 0;
    s_cap_t0     = hal_systick_get_ms();
    s_cap_active = true;   /* on_sample() now stores into s_cap_buf */
    return drv_ads131m04_start();
}

bool svc_displacement_capture_done(void)
{
    return s_cap_done;
}

void svc_displacement_capture_end(void)
{
    if (s_cap_active) {
        uint32_t t1 = s_cap_t1 ? s_cap_t1 : hal_systick_get_ms();
        s_last_samples    = s_cap_idx;
        s_last_drops      = (uint16_t)drv_ads131m04_get_integrity()->ring_overflow;
        s_last_elapsed_ms = t1 - s_cap_t0;
    }
    s_cap_active = false;
    drv_ads131m04_stop();
}

void svc_displacement_last_capture(uint16_t *samples, uint16_t *drops,
                                    uint32_t *elapsed_ms)
{
    if (samples)    *samples    = s_last_samples;
    if (drops)      *drops      = s_last_drops;
    if (elapsed_ms) *elapsed_ms = s_last_elapsed_ms;
}

const uint8_t *svc_displacement_capture_buffer(void)
{
    return s_cap_buf;
}

uint16_t svc_displacement_capture_sample_count(void)
{
    return ADC_BULK_SAMPLE_COUNT;
}

uint16_t svc_displacement_capture_drops(void)
{
    return (uint16_t)drv_ads131m04_get_integrity()->ring_overflow;
}

DrvStatus svc_displacement_phasor_log_begin(void)
{
    if (s_phasor_log_active) {
        return DRV_ERR_NOT_READY;
    }
    /* Same rationale as svc_displacement_start()'s reset: on_sample()
     * only runs once the driver's trigger is armed below, so it's safe
     * to clear the accumulation state here first. */
    accum_reset();

    s_phasor_log_idx         = 0;
    s_phasor_log_done        = false;
    s_phasor_log_decim_count = 0;
    s_phasor_log_active      = true;   /* svc_displacement_update() now routes completed batches here */
    return drv_ads131m04_start();
}

bool svc_displacement_phasor_log_done(void)
{
    return s_phasor_log_done;
}

void svc_displacement_phasor_log_end(void)
{
    s_phasor_log_active = false;
    drv_ads131m04_stop();
}

const DisplacementPhasorLogEntry *svc_displacement_phasor_log_buffer(void)
{
    return s_phasor_log;
}

uint16_t svc_displacement_phasor_log_count(void)
{
    return DISPLACEMENT_PHASOR_LOG_DEPTH;
}

uint16_t svc_displacement_phasor_log_progress(void)
{
    return s_phasor_log_idx;
}

DrvStatus svc_displacement_phasor_stream_begin(void)
{
    if (s_pstream_active || s_phasor_log_active) {
        return DRV_ERR_NOT_READY;
    }
    accum_reset();   /* same reasoning as svc_displacement_phasor_log_begin() */
    s_pstream_head   = 0;
    s_pstream_tail   = 0;
    s_pstream_drops  = 0;
    s_pstream_active = true;   /* svc_displacement_update() now routes batches here */
    DrvStatus rc = drv_ads131m04_start();
    if (rc != DRV_OK) {
        s_pstream_active = false;
    }
    return rc;
}

void svc_displacement_phasor_stream_end(void)
{
    if (!s_pstream_active) return;
    s_pstream_active = false;
    drv_ads131m04_stop();
}

bool svc_displacement_phasor_stream_active(void)
{
    return s_pstream_active;
}

bool svc_displacement_phasor_stream_peek(DisplacementPhasorLogEntry *out)
{
    if (s_pstream_tail == s_pstream_head) return false;
    *out = s_pstream[s_pstream_tail];
    return true;
}

void svc_displacement_phasor_stream_consume(void)
{
    if (s_pstream_tail != s_pstream_head) {
        s_pstream_tail = (uint16_t)((s_pstream_tail + 1U) & PSTREAM_MASK);
    }
}

uint16_t svc_displacement_phasor_stream_drops(void)
{
    return s_pstream_drops;
}

DrvStatus svc_displacement_zero_cal_step1_begin(void)
{
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

bool svc_displacement_zero_cal_consume_result(float *offset1_mm_out, float *offset2_mm_out)
{
    if (s_zero_cal_phase != DISP_ZERO_CAL_RESULT_READY) {
        return false;
    }
    if (offset1_mm_out) *offset1_mm_out = s_zero_cal_result1_mm;
    if (offset2_mm_out) *offset2_mm_out = s_zero_cal_result2_mm;
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
    s_precision_count1      = 0;
    s_precision_count2      = 0;
    s_precision_sum1        = 0.0;
    s_precision_sum2        = 0.0;
    s_precision_count_diff  = 0;
    s_precision_sum_diff    = 0.0;
    s_precision_timed_out   = false;
    s_precision_start_ms    = hal_systick_get_ms();
    s_precision_phase       = DISP_PRECISION_RUNNING;
    return DRV_OK;
}

void svc_displacement_precision_cancel(void)
{
    s_precision_phase      = DISP_PRECISION_IDLE;
    s_precision_count1     = 0;
    s_precision_count2     = 0;
    s_precision_count_diff = 0;
}

DisplacementPrecisionPhase svc_displacement_precision_get_phase(void)
{
    return s_precision_phase;
}

void svc_displacement_precision_progress(uint16_t *count1_out, uint16_t *count2_out,
                                          uint16_t *count_diff_out, uint16_t *target_out,
                                          uint32_t *elapsed_ms_out)
{
    if (count1_out)     *count1_out     = s_precision_count1;
    if (count2_out)     *count2_out     = s_precision_count2;
    if (count_diff_out) *count_diff_out = s_precision_count_diff;
    if (target_out)     *target_out     = DISPLACEMENT_PRECISION_TARGET_SAMPLES;
    if (elapsed_ms_out) {
        *elapsed_ms_out = (s_precision_phase == DISP_PRECISION_IDLE)
            ? 0U : (uint32_t)(hal_systick_get_ms() - s_precision_start_ms);
    }
}

bool svc_displacement_precision_get_result(float *delta1_mm_out, float *delta2_mm_out,
                                            float *delta_diff_mm_out, bool *timed_out_out)
{
    if (s_precision_phase != DISP_PRECISION_DONE) {
        return false;
    }
    /* Same sign-flip-at-the-output-boundary policy as the getters above --
     * s_precision_result*_mm are accumulated upstream (precision_accumulate())
     * in the sensor's native sign convention. delta_diff specifically is
     * its own dedicated quality-gated accumulator (see this function's own
     * header comment / svc_displacement.h), not delta1-delta2 -- scaling it
     * by S1's sign alone is exactly correct when S1/S2 share the same
     * invert setting (the expected case, both rigidly mounted in one
     * housing) and is the best available answer if they don't, short of
     * losing the dedicated accumulator's quality-gating by recomputing it
     * from the two independent results instead. */
    float sign1 = sensor_sign(g_device_settings.disp_s1_invert);
    float sign2 = sensor_sign(g_device_settings.disp_s2_invert);
    if (delta1_mm_out)      *delta1_mm_out      = sign1 * s_precision_result1_mm;
    if (delta2_mm_out)      *delta2_mm_out      = sign2 * s_precision_result2_mm;
    if (delta_diff_mm_out)  *delta_diff_mm_out  = sign1 * s_precision_result_diff_mm;
    if (timed_out_out)      *timed_out_out      = s_precision_timed_out;
    return true;
}
