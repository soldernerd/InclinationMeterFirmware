#include "svc_displacement.h"
#include "svc_displacement_internal.h"
#include "drv_ads131m04.h"
#include "math_phasor.h"
#include "math_window.h"
#include "math_displacement.h"
#include "math_quality.h"
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

/* Sample-callback-only accumulators -- touched only from on_sample(),
 * always called from the same context (drv_ads131m04.c's SysTick frame
 * drain), so no volatile/locking needed here (same reasoning as WP8's
 * svc_signal_analysis.c). */
static uint8_t  s_sample_idx   = 0;                 /* position within the cycle, 0..7 */
static uint8_t  s_batch_cycles = 0;                 /* cycles completed in the batch in progress */
static int32_t  s_pos_sum[4][MATH_PHASOR_SAMPLES_PER_CYCLE];   /* [ADC channel][position] */

/* s_input_drop_count is written from on_sample() and read from
 * svc_displacement_get_input_drop_count() (task context) -- volatile,
 * same reasoning as s_in_head/s_in_tail above. s_degenerate_count is
 * written AND read only from task context, so it doesn't need it. */
static volatile uint16_t s_input_drop_count = 0;
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

/* Latest-batch snapshot for the API v2 Measurements (0x4) GET/SUBSCRIBE
 * resources (Services/svc_api.c) -- kept here, not in g_system_state,
 * same pattern as svc_battery_get_vbat_mv()/svc_powertest_mask() in
 * this codebase: a subsystem that owns values nothing but the API layer
 * reads exposes them via a getter instead of a shared-struct field.
 * Task context only (process_one_batch(), same context as everything
 * else in this file except on_sample()). s_delta*_mm_raw is the per-batch
 * reading (~40.7 Hz, API Topic 0x03); the smoothed reading is the Hann
 * display stream below (the 8-batch moving average this used to carry was
 * retired 2026-10-07). */
static float s_delta1_mm_raw = 0.0f;
static float s_delta2_mm_raw = 0.0f;
static float s_residual1     = 0.0f;
static float s_residual2     = 0.0f;
static bool  s_disp_ok       = false;

/* --- Contiguous batch history + display stream (2026-10-07, config.h's
 * DISPLACEMENT_DISPLAY_TAPS and DISPLACEMENT_QUALITY_K comments) ---
 * One MathWindow (Math/math_window.h) holds the newest 81 batches' readings
 * and squared Im(x) steps per sensor plus the quiet floors. The LIVE display
 * stream is a Hann window over the newest DISPLACEMENT_DISPLAY_TAPS readings,
 * published every DISPLACEMENT_DISPLAY_DECIMATION-th batch, with a per-sensor
 * "doubtful" flag from the window-level quality indicator; the triggered
 * precision measurement uses the whole 81-batch window. Task context only
 * (process_one_batch() writes, the getters read -- same context as everything
 * else here except on_sample()). */
/* Batches per second (the batch rate the floor creep is expressed in): carrier
 * cycles per second / cycles per batch. 2604.1667 / 64 = 40.69 Hz. */
#define BATCH_RATE_HZ  (2604.1667f / (float)DISPLACEMENT_BATCH_CYCLES)
/* The window history, the quiet floors, the Hann display stream and its contiguity
 * tracking live in one pure state machine (Math/math_quality.h, host-tested). */
static MathDisplay s_ds;

/* Latest completed batch's raw phasors -- same storage/context reasoning
 * as the block above, added 2026-09-25 for the API v2 Topic groups (0x5)
 * real-time diagnostic resource. Written alongside s_delta1_mm etc. in
 * process_one_batch(). */
static DisplacementPhasors s_phasors = {0};

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

/* Clears the batch history, the quiet floors and the display stream (task
 * context, demod not running). */
static void display_reset(void)
{
    math_display_reset(&s_ds);
}

static void note_saturating(volatile uint16_t *counter)
{
    if (*counter < UINT16_MAX) {
        (*counter)++;
    }
}

static void on_sample(int32_t ch0, int32_t ch1, int32_t ch2, int32_t ch3)
{
    /* Bulk-capture mode (svc_disp_capture.c): store the raw codes and get out; the phasor
     * accumulation below is skipped so this stays trivially cheap for the ~0.3 s a capture
     * runs. Mutually exclusive with the demod path by construction -- svc_api's bulk dispatch
     * only arms this while svc_displacement_is_running() is false. */
    if (g_disp_cap_active) {
        disp_capture_store(ch0, ch1, ch2, ch3);
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
         * overwrite one it hasn't read yet. Evicting the oldest instead
         * would violate this ring's single-writer invariant: s_in_tail is
         * task-owned (only svc_displacement_update() writes it). Counted in cycles (the unit this counter always had); the
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

/* cos/sin of the phase calibration, cached per sensor: the angle changes
 * only when a host SETs it, and sinf/cosf are soft-float library calls on
 * this Cortex-M0+ that must not run every 24.6 ms batch. */
typedef struct { int16_t cdeg; float c, s; bool valid; } PhaseCache;
static PhaseCache s_phase_cache[2];

/* Builds one sensor's calibration (DeviceSettings' EEPROM-backed scaled integers
 * -> floats; the arithmetic itself is Math/math_displacement.c). */
static void load_sensor_cal(MathSensorCal *out, int32_t k_micro, uint32_t pga,
                            int32_t zero_ppm, int16_t phase_cdeg, uint8_t sensor_idx)
{
    PhaseCache *pc = &s_phase_cache[sensor_idx];
    if (!pc->valid || pc->cdeg != phase_cdeg) {
        math_phase_sincos(phase_cdeg, &pc->c, &pc->s);
        pc->cdeg  = phase_cdeg;
        pc->valid = true;
    }
    math_sensor_cal_make(out, k_micro, pga, zero_ppm, pc->c, pc->s);
}

/* One batch's phasors: DISPLACEMENT_BATCH_CYCLES consecutive cycles' raw I/Q,
 * coherently summed (config.h / docs/decisions.md have the story on why the
 * division-heavy math runs once per batch). */
typedef MathBatchSums BatchSums;

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
    return math_phasor_exceeds(i_sum, q_sum, DISPLACEMENT_MAX_THEORETICAL_PHASOR_MAG);
}

static void process_one_batch(const BatchSums *s, uint16_t seq)
{
    /* Stash the raw phasors first -- a diagnostic host looking at WHY the
     * excitation phasors are degenerate needs this snapshot precisely when the
     * delta math itself can't produce a reading. */
    s_phasors.iB = (float)s->iB;  s_phasors.qB = (float)s->qB;
    s_phasors.iA = (float)s->iA;  s_phasors.qA = (float)s->qA;
    s_phasors.iS1 = (float)s->iS1;  s_phasors.qS1 = (float)s->qS1;
    s_phasors.iS2 = (float)s->iS2;  s_phasors.qS2 = (float)s->qS2;

    if (phasor_exceeds_theoretical_max(s->iB, s->qB)
        || phasor_exceeds_theoretical_max(s->iA, s->qA)
        || phasor_exceeds_theoretical_max(s->iS1, s->qS1)
        || phasor_exceeds_theoretical_max(s->iS2, s->qS2)) {
        note_saturating(&s_amplitude_fault_count);
    }

    MathSensorCal cal[2];
    load_sensor_cal(&cal[0], g_device_settings.disp_s1_k_micro, ADS131M04_PGA_S1,
                    g_device_settings.disp_s1_zero_ppm, g_device_settings.disp_s1_phase_cdeg, 0U);
    load_sensor_cal(&cal[1], g_device_settings.disp_s2_k_micro, ADS131M04_PGA_S2,
                    g_device_settings.disp_s2_zero_ppm, g_device_settings.disp_s2_phase_cdeg, 1U);

    MathBatchResult res;
    if (!math_batch_demod(s, cal, &res)) {
        /* A and B exactly identical -- degenerate excitation, shouldn't happen in
         * practice. Skip rather than divide by zero; both sensors share the (A-B)
         * denominator so it is degenerate for both. */
        note_saturating(&s_degenerate_count);
        math_display_skip(&s_ds);   /* the next batch must not join this window */
        s_disp_ok = false;          /* no valid reading from this batch; the next good one sets it again */
        return;
    }

    /* Contiguity: consecutive batches are exactly DISPLACEMENT_BATCH_CYCLES cycles
     * apart. A dropped batch (or a restart) forgets the windows, and a precision
     * measurement in progress starts filling again. */
    if (!math_display_feed(&s_ds, seq, (uint16_t)DISPLACEMENT_BATCH_CYCLES, res.delta, res.residual)) {
        disp_procs_break();
    }

    s_delta1_mm_raw = res.delta[0];
    s_delta2_mm_raw = res.delta[1];
    s_residual1 = res.residual[0];
    s_residual2 = res.residual[1];
    s_disp_ok   = true;

    disp_procs_batch(&s_ds, res.delta[0], res.delta[1]);
}

DrvStatus svc_displacement_init(void)
{
    /* creep per batch so that the floor may double in DISPLACEMENT_QUALITY_FLOOR_DOUBLING_S
     * if every window is noisier than it: ln2 / (seconds * batches per second) */
    math_display_init(&s_ds, DISPLACEMENT_DISPLAY_TAPS, DISPLACEMENT_DISPLAY_DECIMATION,
                      DISPLACEMENT_PRECISION_WINDOW_BATCHES, DISPLACEMENT_QUALITY_K,
                      0.693147f / ((float)DISPLACEMENT_QUALITY_FLOOR_DOUBLING_S * BATCH_RATE_HZ),
                      (uint32_t)((float)DISPLACEMENT_QUALITY_RESEED_S * BATCH_RATE_HZ));
    disp_procs_init();
    accum_reset();
    s_input_drop_count  = 0;
    s_degenerate_count  = 0;
    s_clip_count             = 0;
    s_amplitude_fault_count  = 0;
    s_clip_logged            = false;
    s_amplitude_fault_logged = false;
    s_disp_ok           = false;
    display_reset();

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
    /* Idempotent for real (2026-10-07): a start while the acquisition is
     * already running must not touch anything. The reset below runs in task
     * context while on_sample() is live in the SysTick drain, so repeating it
     * would zero a batch mid-sum, race the ring indices, and silently cancel a
     * running zero-cal / precision measurement and forget the quiet floor. */
    if (drv_ads131m04_is_running()) {
        return DRV_OK;
    }
    /* Reset the rings and the sample-callback accumulators so the first
     * cycles after a start are clean, not a stale mix left over from a
     * previous run -- on_sample() only ever runs while the driver's
     * trigger is armed, so it's safe to touch its state here (task
     * context) before arming it. */
    accum_reset();
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
    display_reset();
    /* A fresh start invalidates any in-progress zero-cal run -- its
     * averaging assumed a continuous demod session, not one straddling a
     * stop/start. Same for a precision measurement -- its averaging
     * assumed a continuous session too. */
    svc_displacement_zero_cal_cancel();
    svc_displacement_precision_cancel();
    return drv_ads131m04_start();
}

/* The polled / subscribed Measurements values are the Hann display value once
 * it exists (first one ~0.6 s after a start), the raw batch value until then. */
float svc_displacement_get_delta1_mm(void)
{
    return disp_sensor_sign(g_device_settings.disp_s1_invert) * (s_ds.valid ? s_ds.out[0] : s_delta1_mm_raw);
}
float svc_displacement_get_residual1(void) { return s_residual1; }
float svc_displacement_get_delta2_mm(void)
{
    return disp_sensor_sign(g_device_settings.disp_s2_invert) * (s_ds.valid ? s_ds.out[1] : s_delta2_mm_raw);
}
float svc_displacement_get_residual2(void) { return s_residual2; }
bool  svc_displacement_get_ok(void)        { return s_disp_ok; }

float svc_displacement_get_display_delta1_mm(void) { return disp_sensor_sign(g_device_settings.disp_s1_invert) * s_ds.out[0]; }
float svc_displacement_get_display_delta2_mm(void) { return disp_sensor_sign(g_device_settings.disp_s2_invert) * s_ds.out[1]; }
float svc_displacement_get_display_delta_diff_mm(void)
{
    return svc_displacement_get_display_delta1_mm() - svc_displacement_get_display_delta2_mm();
}
bool     svc_displacement_get_display_doubtful1(void) { return s_ds.doubtful[0]; }
bool     svc_displacement_get_display_doubtful2(void) { return s_ds.doubtful[1]; }
bool     svc_displacement_get_display_doubtful_diff(void) { return s_ds.doubtful[0] || s_ds.doubtful[1]; }
uint16_t svc_displacement_get_display_seq(void)   { return s_ds.seq; }
bool     svc_displacement_get_display_valid(void) { return s_disp_ok && s_ds.valid; }

float svc_displacement_get_delta1_mm_raw(void) { return disp_sensor_sign(g_device_settings.disp_s1_invert) * s_delta1_mm_raw; }
float svc_displacement_get_delta2_mm_raw(void) { return disp_sensor_sign(g_device_settings.disp_s2_invert) * s_delta2_mm_raw; }

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

/* Window-level verdict of the display stream (the old per-batch residual-step
 * flag was retired 2026-10-07): "ok" = the newest display window was not
 * doubtful. */
bool svc_displacement_get_quality1_ok(void) { return !s_ds.doubtful[0]; }
bool svc_displacement_get_quality2_ok(void) { return !s_ds.doubtful[1]; }
bool svc_displacement_get_quality_diff_ok(void) { return !s_ds.doubtful[0] && !s_ds.doubtful[1]; }

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
 * (full scale +-1.2V/PGA at +-ADS131M04_CODE_MAX, i.e. 1 LSB = 2.4V/PGA/2^24,
 * drv_ads131m04.h).
 *
 * FIXED 2026-10-06: this used 2400/PGA/CODE_MAX, i.e. twice the datasheet
 * LSB (the 2.4 V span is +-1.2 V over 2^24 codes, not over 2^23). Found in
 * the signal-processing note (docs/signal_processing.tex, Sec. 7: channel A
 * read 1.25 V peak on a +-1.2 V range without clipping). It scaled the
 * DIAGNOSTICS screen, the API signal-diagnostic RMS/P2P values and the
 * "theoretical tilt" 2x too high; deltas and the sensitivity-based
 * calibration do not use this function. */
static float phasor_peak_mv(float i, float q, uint8_t pga)
{
    float mag       = sqrtf(i * i + q * q);
    float peak_code = mag / (65536.0f * (float)DISPLACEMENT_BATCH_CYCLES);
    return peak_code * (1200.0f / (float)pga) / (float)ADS131M04_CODE_MAX;
}

void svc_displacement_get_signal_diag(DisplacementSignalDiag *out)
{
    if (out == 0) {
        return;
    }
    const float i[4] = { s_phasors.iB, s_phasors.iA, s_phasors.iS1, s_phasors.iS2 };
    const float q[4] = { s_phasors.qB, s_phasors.qA, s_phasors.qS1, s_phasors.qS2 };
    /* B, A, S1, S2 -- single source of truth: drv_ads131m04.h. */
    const uint8_t pga[4] = { ADS131M04_PGA_B, ADS131M04_PGA_A, ADS131M04_PGA_S1, ADS131M04_PGA_S2 };

    /* Phase reference: D = A - B at 90 deg (Sec. 9.1) -- subtract D's own
     * angle, so the display no longer depends on the sample-grid phase. */
    const float psi_d = atan2f(q[1] - q[0], i[1] - i[0]) * (180.0f / 3.14159265f);

    for (uint8_t ch = 0; ch < 4U; ++ch) {
        float peak_mv    = phasor_peak_mv(i[ch], q[ch], pga[ch]);
        out->rms_mv[ch]   = peak_mv * 0.70710678f;   /* /sqrt(2) */
        out->p2p_mv[ch]   = peak_mv * 2.0f;
        float ph = atan2f(q[ch], i[ch]) * (180.0f / 3.14159265f) - psi_d + 90.0f;
        while (ph < 0.0f)    { ph += 360.0f; }
        while (ph >= 360.0f) { ph -= 360.0f; }
        out->phase_deg[ch] = ph;
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

        /* The continuous phasor stream (svc_displacement_phasor_stream_begin())
         * is a tap on the running measurement: it copies each batch's raw sums out
         * and the demod runs as usual, so the LIVE screen / API readings stay valid
         * while a host logs the phasors. */
        if (g_disp_pstream_active) {
            disp_pstream_store(&sums, batch_seq);
        }
        process_one_batch(&sums, batch_seq);
    }

    /* Precision-measurement timeout, checked every tick regardless of whether a batch
     * completed this pass -- this is what guarantees the error still fires if acquisition
     * stalls (see config.h's DISPLACEMENT_PRECISION_TIMEOUT_MS comment). */
    disp_procs_poll();
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

    /* Stop through the service, not just the driver: the displacement reading
     * must not stay "valid" with frozen values, and a running zero-cal /
     * precision measurement must not hang in its RUNNING phase. */
    g_disp_pstream_active = false;
    svc_displacement_stop();
}

void svc_displacement_clear_counters(void)
{
    s_input_drop_count         = 0;
    s_degenerate_count         = 0;
    s_clip_count               = 0;
    s_amplitude_fault_count    = 0;
    s_clip_logged              = false;
    s_amplitude_fault_logged   = false;
    s_max_update_gap_ms        = 0;
    s_max_gap_at_uptime_ms     = 0;
    s_gap_over_threshold_count = 0;
    disp_pstream_clear_drops();
}

uint16_t svc_displacement_get_input_drop_count(void)
{
    return s_input_drop_count;
}

uint16_t svc_displacement_get_output_drop_count(void)
{
    return 0;   /* the output ring was removed 2026-10-07; the wire field stays */
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

