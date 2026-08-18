#include "svc_displacement.h"
#include "drv_ads131m04.h"
#include "math_phasor.h"
#include "config.h"
#include "system_state.h"
#include <stddef.h>
#include <stdbool.h>

/* Channel mapping (confirmed 2026-08-18 -- no existing hardware doc
 * states this, recorded here and in config.h's WP10 comment):
 *   ch0 = B, ch1 = A (antiphase excitation pair, shared by both heads)
 *   ch2 = S1, ch3 = S2 (each head's pendulum/center-plate pickup)
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
    int64_t iB, qB, iA, qA, iS1, qS1, iS2, qS2;
} RawCycle;

/* Producer (ISR) -> consumer (task) handoff, one entry per completed
 * carrier cycle -- lock-free SPSC, power-of-two size, drop-new-on-full
 * (CLAUDE.md 8.3). head is ISR-owned (only on_sample() writes it), tail
 * is task-owned (only svc_displacement_update() writes it); each side
 * only reads the other's index, so no locking is needed beyond the
 * volatile qualifier. */
static RawCycle          s_in_ring[DISPLACEMENT_RING_DEPTH];
static volatile uint16_t s_in_head = 0;
static volatile uint16_t s_in_tail = 0;

/* Task (producer) -> svc_displacement_pop() caller (consumer) handoff --
 * same SPSC/power-of-two/drop-new-on-full shape, one entry per computed
 * result. This is the "raw per-cycle delta stream" this module is
 * required to retain, not just the latest g_system_state snapshot. */
static DisplacementCycle s_out_ring[DISPLACEMENT_RING_DEPTH];
static volatile uint16_t s_out_head = 0;
static volatile uint16_t s_out_tail = 0;

/* ISR-only accumulators -- touched only from on_sample(), always called
 * from the same interrupt context (see drv_ads131m04.h), so no
 * volatile/locking needed here (same reasoning as WP8's
 * svc_signal_analysis.c). */
static uint8_t s_sample_idx = 0;
static int64_t s_iB, s_qB, s_iA, s_qA, s_iS1, s_qS1, s_iS2, s_qS2;

/* s_input_drop_count is written from on_sample() (ISR context) and read
 * from svc_displacement_get_input_drop_count() (task context) -- volatile,
 * same reasoning as s_in_head/s_in_tail above. s_output_drop_count and
 * s_degenerate_count are both written AND read only from task context
 * (process_one_cycle()/their getters), so they don't need it. */
static volatile uint16_t s_input_drop_count = 0;
static uint16_t s_output_drop_count = 0;
static uint16_t s_degenerate_count  = 0;

static void note_saturating(volatile uint16_t *counter)
{
    if (*counter < UINT16_MAX) {
        (*counter)++;
    }
}

static void on_sample(int32_t ch0, int32_t ch1, int32_t ch2, int32_t ch3)
{
    math_phasor_accumulate(ch0, s_sample_idx, &s_iB, &s_qB);
    math_phasor_accumulate(ch1, s_sample_idx, &s_iA, &s_qA);
    math_phasor_accumulate(ch2, s_sample_idx, &s_iS1, &s_qS1);
    math_phasor_accumulate(ch3, s_sample_idx, &s_iS2, &s_qS2);

    s_sample_idx++;
    if (s_sample_idx < MATH_PHASOR_SAMPLES_PER_CYCLE) {
        return;
    }
    s_sample_idx = 0;

    uint16_t head = s_in_head;
    uint16_t next = (uint16_t)((head + 1U) & RING_MASK);
    if (next == s_in_tail) {
        /* Consumer isn't keeping up -- drop this cycle rather than
         * overwrite one it hasn't read yet. DELIBERATELY the opposite
         * policy from push_output() below: s_in_tail is task-owned
         * (svc_displacement_update() is the only writer), so evicting
         * it from here (ISR/producer context) the way push_output()
         * evicts s_out_tail would violate this ring's single-writer
         * invariant for the tail index and open an ISR/task race. If
         * this block is ever refactored into a named push_input()
         * helper mirroring push_output()'s shape, do NOT copy
         * push_output()'s eviction branch across. */
        note_saturating(&s_input_drop_count);
    } else {
        s_in_ring[head].iB  = s_iB;  s_in_ring[head].qB  = s_qB;
        s_in_ring[head].iA  = s_iA;  s_in_ring[head].qA  = s_qA;
        s_in_ring[head].iS1 = s_iS1; s_in_ring[head].qS1 = s_qS1;
        s_in_ring[head].iS2 = s_iS2; s_in_ring[head].qS2 = s_qS2;
        s_in_head = next;
    }

    s_iB = s_qB = s_iA = s_qA = s_iS1 = s_qS1 = s_iS2 = s_qS2 = 0;
}

static void push_output(float delta1, float residual1, float delta2, float residual2)
{
    uint16_t head = s_out_head;
    uint16_t next = (uint16_t)((head + 1U) & RING_MASK);
    if (next == s_out_tail) {
        /* Full -- overwrite the oldest unread entry rather than drop the
         * newest. Deliberately the opposite policy from the input ring
         * above (and from CLAUDE.md 8.3's comms-transport convention,
         * where dropping new preserves in-order delivery for a peer
         * actively reading): nothing consumes this output ring yet (see
         * svc_displacement_pop()'s doc comment), so drop-new would mean
         * the buffer permanently freezes at whatever the first
         * DISPLACEMENT_RING_DEPTH cycles after boot were and never
         * updates again -- useless as a "retained recent stream" once a
         * consumer eventually shows up. Overwrite-oldest keeps this
         * always holding the most recent ~24.6 ms of results, which is
         * the useful behavior for a buffer nobody may be actively
         * draining. */
        s_out_tail = (uint16_t)((s_out_tail + 1U) & RING_MASK);
        note_saturating(&s_output_drop_count);
    }
    s_out_ring[head].delta1_mm = delta1;
    s_out_ring[head].residual1 = residual1;
    s_out_ring[head].delta2_mm = delta2;
    s_out_ring[head].residual2 = residual2;
    s_out_head = next;
}

/* The values every sensor's computation needs but that don't vary
 * between sensors within one cycle -- bundled so process_one_cycle()'s
 * two compute_sensor_delta() calls below can't have same-typed adjacent
 * float arguments transposed by a future edit to one call site but not
 * the other (a round-2 code-review finding: C gives no type-based
 * protection against swapping e.g. iB/qB or inv_den_re/inv_den_im when
 * they're passed as bare positional floats).
 *
 * inv_den_re/inv_den_im is 1/(A-B), already inverted -- both sensors
 * divide by the identical (A-B) denominator, so process_one_cycle()
 * computes that one reciprocal once via math_complex_reciprocal() and
 * both calls below multiply by it instead of each dividing separately
 * (division is the most expensive op available; multiplication is
 * much cheaper). */
typedef struct {
    float atten;
    float iB, qB;
    float inv_den_re, inv_den_im;   /* 1 / (A - B) */
} SharedCycleTerms;

/* One sensor's x/delta/residual, given the shared (A-B) reciprocal --
 * factored out so process_one_cycle() below computes S1 and S2 the same
 * way instead of two hand-duplicated copies (WP8's svc_signal_analysis.c,
 * this module's predecessor, looped over channels for the same kind of
 * repeated per-channel math). Cannot fail -- the only degenerate case
 * (A-B exactly zero) is checked once in process_one_cycle() before this
 * is called, since it's identical for both sensors. */
static void compute_sensor_delta(float iS, float qS, const DisplacementSensorCal *cal,
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

static void process_one_cycle(const RawCycle *c)
{
    float iB = (float)c->iB, qB = (float)c->qB;
    float iA = (float)c->iA, qA = (float)c->qA;

    /* 1/(A - B), the shared denominator's reciprocal -- computed once and
     * reused by both sensors below (see SharedCycleTerms's comment). */
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

    SharedCycleTerms shared = {
        .atten      = g_disp_shared_cal.atten,
        .iB         = iB,
        .qB         = qB,
        .inv_den_re = inv_den_re,
        .inv_den_im = inv_den_im,
    };

    float delta1, residual1, delta2, residual2;
    compute_sensor_delta((float)c->iS1, (float)c->qS1, &g_disp_s1_cal,
                          &shared, &delta1, &residual1);
    compute_sensor_delta((float)c->iS2, (float)c->qS2, &g_disp_s2_cal,
                          &shared, &delta2, &residual2);

    push_output(delta1, residual1, delta2, residual2);

    g_system_state.disp1_delta_mm = delta1;
    g_system_state.disp1_residual = residual1;
    g_system_state.disp2_delta_mm = delta2;
    g_system_state.disp2_residual = residual2;
    g_system_state.disp_ok        = true;
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
    g_system_state.disp_ok = false;

    drv_ads131m04_set_on_sample(on_sample);
    return drv_ads131m04_init();
}

void svc_displacement_update(void)
{
    /* Drain everything queued since the last call -- at ~2.6 kHz
     * production, a task called only every task_sensors_ms (~100 ms)
     * would let hundreds of cycles pile up, so unlike most other
     * scheduler tasks this one is wired to run every tick (see
     * App/app_scheduler.c). */
    while (s_in_tail != s_in_head) {
        process_one_cycle(&s_in_ring[s_in_tail]);
        s_in_tail = (uint16_t)((s_in_tail + 1U) & RING_MASK);
    }
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
