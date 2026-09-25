#include "svc_displacement.h"
#include "drv_ads131m04.h"
#include "math_phasor.h"
#include "svc_log.h"
#include "hal_systick.h"
#include "config.h"
#include "system_state.h"
#include <stddef.h>
#include <stdbool.h>

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
 * per-sample transform (math_phasor_accumulate(), same
 * MATH_PHASOR_SAMPLES_PER_CYCLE/Q14 scale for all four), so that shared
 * digital scale factor cancels in this ratio -- the raw int64 I/Q sums
 * can be used directly (just cast to float), no separate normalization
 * step needed. */

#define RING_MASK  (DISPLACEMENT_RING_DEPTH - 1U)
#if (DISPLACEMENT_RING_DEPTH & RING_MASK) != 0
#error "DISPLACEMENT_RING_DEPTH must be a power of two"
#endif

typedef struct {
    int64_t  iB, qB, iA, qA, iS1, qS1, iS2, qS2;
    uint16_t seq;
} RawCycle;

/* Producer (sample callback) -> consumer (task) handoff, one entry per
 * completed carrier cycle -- lock-free SPSC, power-of-two size,
 * drop-new-on-full (CLAUDE.md 8.3). head is producer-owned (only
 * on_sample() writes it), tail is task-owned (only
 * svc_displacement_update() writes it); each side only reads the other's
 * index, so no locking is needed beyond the volatile qualifier. */
static RawCycle          s_in_ring[DISPLACEMENT_RING_DEPTH];
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
static uint8_t s_sample_idx = 0;
static int64_t s_iB, s_qB, s_iA, s_qA, s_iS1, s_qS1, s_iS2, s_qS2;

/* s_input_drop_count is written from on_sample() and read from
 * svc_displacement_get_input_drop_count() (task context) -- volatile,
 * same reasoning as s_in_head/s_in_tail above. s_output_drop_count and
 * s_degenerate_count are both written AND read only from task context,
 * so they don't need it. */
static volatile uint16_t s_input_drop_count = 0;
static uint16_t s_output_drop_count = 0;
static uint16_t s_degenerate_count  = 0;

/* Written/read only from task context (svc_displacement_start()/
 * svc_displacement_check_integrity()) -- no volatile needed. */
static bool s_fault_reported = false;

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
 * (DISPLACEMENT_MAX_CYCLES_PER_TICK's comment has the full story, a
 * livelock, not a memory bug) and g_system_state would very likely have
 * been fine -- but the getter pattern is a reasonable fit regardless,
 * so it stayed.) Task context only (process_one_batch(), same context
 * as everything else in this file except on_sample()). */
static float s_delta1_mm  = 0.0f;
static float s_residual1  = 0.0f;
static float s_delta2_mm  = 0.0f;
static float s_residual2  = 0.0f;
static bool  s_disp_ok    = false;

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

/* Same reasoning as s_sample_idx above -- assigned in on_sample() to
 * every completed 8-sample cycle *before* the input-ring-full check, so
 * a cycle dropped there (consumer not keeping up) still consumes a seq
 * value and shows up as a gap downstream.
 *
 * Rolls over at 65536 cycles (~25 s at ~2.6 kHz) -- a future stream
 * consumer doing gap detection MUST compare seq values with
 * wraparound-safe (modular) arithmetic, e.g. `(uint16_t)(seq -
 * expected) != 0`, not a naive `seq != prev + 1` or `seq < prev`. */
static uint16_t s_cycle_seq = 0;

/* Task-context batch accumulator -- touched only from
 * svc_displacement_update(), which sums DISPLACEMENT_BATCH_CYCLES
 * consecutive dequeued RawCycles here before handing the total to
 * process_one_batch() (config.h's DISPLACEMENT_BATCH_CYCLES comment has
 * the root-caused reason this exists). s_batch_seq is the most recent
 * cycle folded into the batch so far -- used as the batch's seq. */
static int64_t s_batch_iB, s_batch_qB, s_batch_iA, s_batch_qA;
static int64_t s_batch_iS1, s_batch_qS1, s_batch_iS2, s_batch_qS2;
static uint8_t  s_batch_count = 0;
static uint16_t s_batch_seq   = 0;

static void batch_reset(void)
{
    s_batch_iB = s_batch_qB = s_batch_iA = s_batch_qA = 0;
    s_batch_iS1 = s_batch_qS1 = s_batch_iS2 = s_batch_qS2 = 0;
    s_batch_count = 0;
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

    /* ch0=S2, ch1=B, ch2=A, ch3=S1 -- see this file's top comment. */
    math_phasor_accumulate(ch1, s_sample_idx, &s_iB,  &s_qB);
    math_phasor_accumulate(ch2, s_sample_idx, &s_iA,  &s_qA);
    math_phasor_accumulate(ch3, s_sample_idx, &s_iS1, &s_qS1);
    math_phasor_accumulate(ch0, s_sample_idx, &s_iS2, &s_qS2);

    s_sample_idx++;
    if (s_sample_idx < MATH_PHASOR_SAMPLES_PER_CYCLE) {
        return;
    }
    s_sample_idx = 0;
    uint16_t seq = s_cycle_seq++;

    uint16_t head = s_in_head;
    uint16_t next = (uint16_t)((head + 1U) & RING_MASK);
    if (next == s_in_tail) {
        /* Consumer isn't keeping up -- drop this cycle rather than
         * overwrite one it hasn't read yet. DELIBERATELY the opposite
         * policy from push_output() below: s_in_tail is task-owned (only
         * svc_displacement_update() writes it), so evicting it from here
         * would violate this ring's single-writer invariant for the tail
         * index. */
        note_saturating(&s_input_drop_count);
    } else {
        s_in_ring[head].iB  = s_iB;  s_in_ring[head].qB  = s_qB;
        s_in_ring[head].iA  = s_iA;  s_in_ring[head].qA  = s_qA;
        s_in_ring[head].iS1 = s_iS1; s_in_ring[head].qS1 = s_qS1;
        s_in_ring[head].iS2 = s_iS2; s_in_ring[head].qS2 = s_qS2;
        s_in_ring[head].seq = seq;
        s_in_head = next;
    }

    s_iB = s_qB = s_iA = s_qA = s_iS1 = s_qS1 = s_iS2 = s_qS2 = 0;
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
    float d0_mm;              /* neutral-position air gap, mm */
    float zero_offset_mm;    /* displacement zero calibration, mm */
} SensorCalF;

static void load_sensor_cal(SensorCalF *out, int32_t gain_milli, int32_t d0_um, int32_t zero_offset_um)
{
    out->gain           = (float)gain_milli / 1000.0f;
    out->d0_mm          = (float)d0_um / 1000.0f;
    out->zero_offset_mm = (float)zero_offset_um / 1000.0f;
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
    float iB, qB;
    float inv_den_re, inv_den_im;   /* 1 / (A - B) */
} SharedCycleTerms;

/* One sensor's x/delta/residual, given the shared (A-B) reciprocal --
 * factored out so process_one_batch() below computes S1 and S2 the same
 * way instead of two hand-duplicated copies. Cannot fail -- the only
 * degenerate case (A-B exactly zero) is checked once in
 * process_one_batch() before this is called, since it's identical for
 * both sensors. */
static void compute_sensor_delta(float iS, float qS, const SensorCalF *cal,
                                  const SharedCycleTerms *shared,
                                  float *delta_out, float *residual_out)
{
    /* S/k - B -- S*inv_k is a real-scalar reciprocal-multiply (k =
     * atten*gain has no imaginary part), not a complex operation. */
    float inv_k = 1.0f / (shared->atten * cal->gain);
    float num_re = (iS * inv_k) - shared->iB;
    float num_im = (qS * inv_k) - shared->qB;

    /* x = num * (1/den) -- complex multiply by the precomputed shared
     * reciprocal, equivalent to num/den but without a division here. */
    float x_re = num_re * shared->inv_den_re - num_im * shared->inv_den_im;
    float x_im = num_re * shared->inv_den_im + num_im * shared->inv_den_re;

    *delta_out    = 2.0f * cal->d0_mm * (x_re - 0.5f) - cal->zero_offset_mm;
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
                     g_device_settings.disp_s1_d0_um, g_device_settings.disp_s1_zero_offset_um);
    load_sensor_cal(&s2_cal, g_device_settings.disp_s2_gain_milli,
                     g_device_settings.disp_s2_d0_um, g_device_settings.disp_s2_zero_offset_um);

    SharedCycleTerms shared = {
        .atten      = (float)g_device_settings.disp_atten_milli / 1000.0f,
        .iB         = iB,
        .qB         = qB,
        .inv_den_re = inv_den_re,
        .inv_den_im = inv_den_im,
    };
    float delta1, residual1, delta2, residual2;
    compute_sensor_delta((float)s->iS1, (float)s->qS1, &s1_cal, &shared, &delta1, &residual1);
    compute_sensor_delta((float)s->iS2, (float)s->qS2, &s2_cal, &shared, &delta2, &residual2);

    push_output(seq, delta1, residual1, delta2, residual2);

    s_delta1_mm = delta1;
    s_residual1 = residual1;
    s_delta2_mm = delta2;
    s_residual2 = residual2;
    s_disp_ok   = true;
}

DrvStatus svc_displacement_init(void)
{
    s_sample_idx = 0;
    s_iB = s_qB = s_iA = s_qA = s_iS1 = s_qS1 = s_iS2 = s_qS2 = 0;
    s_in_head  = s_in_tail  = 0;
    s_out_head = s_out_tail = 0;
    s_input_drop_count  = 0;
    s_output_drop_count = 0;
    s_degenerate_count  = 0;
    s_cycle_seq         = 0;
    s_disp_ok           = false;
    batch_reset();
    s_batch_seq = 0;

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
    /* Reset both rings and the sample-callback accumulator so the first
     * cycles after a start are clean, not a stale mix left over from a
     * previous run -- on_sample() only ever runs while the driver's
     * trigger is armed, so it's safe to touch its state here (task
     * context) before arming it. */
    s_sample_idx = 0;
    s_iB = s_qB = s_iA = s_qA = s_iS1 = s_qS1 = s_iS2 = s_qS2 = 0;
    s_in_head  = s_in_tail  = 0;
    s_out_head = s_out_tail = 0;
    s_cycle_seq = 0;
    s_fault_reported = false;
    s_disp_ok = false;
    batch_reset();
    s_batch_seq = 0;
    return drv_ads131m04_start();
}

float svc_displacement_get_delta1_mm(void) { return s_delta1_mm; }
float svc_displacement_get_residual1(void) { return s_residual1; }
float svc_displacement_get_delta2_mm(void) { return s_delta2_mm; }
float svc_displacement_get_residual2(void) { return s_residual2; }
bool  svc_displacement_get_ok(void)        { return s_disp_ok; }

void svc_displacement_get_phasors(DisplacementPhasors *out)
{
    if (out != 0) {
        *out = s_phasors;
    }
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
    s_in_head = s_in_tail = 0;
    batch_reset();
    s_batch_seq = 0;
    /* disp_ok reports "have a currently-valid measurement", not just
     * "the last batch before stop succeeded" -- clear it so a host
     * reading Measurements 0x0D right after a stop doesn't see a stale
     * ok=1 from before acquisition was halted. */
    s_disp_ok = false;
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

void svc_displacement_update(void)
{
    /* Drain what's queued since the last call, folding
     * DISPLACEMENT_BATCH_CYCLES raw cycles into one coherent sum before
     * running process_one_batch()'s division-heavy math once per batch
     * (config.h's DISPLACEMENT_BATCH_CYCLES comment has the full
     * root-caused reasoning). Bounded to at most
     * DISPLACEMENT_MAX_CYCLES_PER_TICK dequeues per call -- a real,
     * reproduced bug (2026-09-24, see the same comment) was this loop
     * running unbounded: if on_sample() (ISR context, ~2.6 kHz) ever
     * queues cycles faster than this can drain them, an unbounded
     * `while (s_in_tail != s_in_head)` never exits, and the scheduler's
     * main loop never returns to run anything else again -- confirmed
     * with a debugger, not a HardFault, a genuine livelock. This cap
     * turns that failure mode into ordinary graceful drops
     * (s_input_drop_count, already handled) instead. */
    uint8_t drained = 0;
    while (s_in_tail != s_in_head && drained < DISPLACEMENT_MAX_CYCLES_PER_TICK) {
        const RawCycle *c = &s_in_ring[s_in_tail];
        s_batch_iB  += c->iB;  s_batch_qB  += c->qB;
        s_batch_iA  += c->iA;  s_batch_qA  += c->qA;
        s_batch_iS1 += c->iS1; s_batch_qS1 += c->qS1;
        s_batch_iS2 += c->iS2; s_batch_qS2 += c->qS2;
        s_batch_seq  = c->seq;
        s_in_tail = (uint16_t)((s_in_tail + 1U) & RING_MASK);
        drained++;

        if (++s_batch_count >= DISPLACEMENT_BATCH_CYCLES) {
            BatchSums sums = {
                .iB = s_batch_iB, .qB = s_batch_qB, .iA = s_batch_iA, .qA = s_batch_qA,
                .iS1 = s_batch_iS1, .qS1 = s_batch_qS1, .iS2 = s_batch_iS2, .qS2 = s_batch_qS2,
            };
            /* A phasor-log capture (svc_displacement_phasor_log_begin(),
             * config.h's "Displacement phasor diagnostics" comment) wants
             * the raw batch sums stored, not demodulated -- same
             * accumulation/batching pipeline either way, this is the only
             * fork point. */
            if (s_phasor_log_active) {
                store_phasor_log_entry(&sums, s_batch_seq);
            } else {
                process_one_batch(&sums, s_batch_seq);
            }
            batch_reset();
        }
    }
}

void svc_displacement_check_integrity(void)
{
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
    s_sample_idx = 0;
    s_iB = s_qB = s_iA = s_qA = s_iS1 = s_qS1 = s_iS2 = s_qS2 = 0;
    s_in_head  = s_in_tail  = 0;
    s_cycle_seq = 0;
    batch_reset();
    s_batch_seq = 0;

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
