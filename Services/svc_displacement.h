#ifndef SVC_DISPLACEMENT_H
#define SVC_DISPLACEMENT_H

#include <stdint.h>
#include <stdbool.h>
#include "drv_common.h"

/* Differential-capacitor displacement sensors S1/S2 (WP10, REV B port
 * 2026-09-24 of the never-merged `wp10` branch's design -- the math and
 * ISR/task split are unchanged from there; only the ADS131M04 channel
 * mapping is REV B's own, confirmed with the user rather than carried
 * over from wp10's REV A mapping (they differ). Replaces
 * Services/svc_signal_analysis.c (WP8's "first cut... additional math
 * may follow" generic 4-channel amplitude/phase diagnostic) as the sole
 * consumer of Drivers_App/drv_ads131m04.c's per-sample callback -- the
 * driver only supports one callback at a time.
 *
 * REV B channel mapping (confirmed with the user 2026-09-24, no
 * hardware doc states this otherwise):
 *   CH0 = Sensor 2 (S2)      CH1 = Exciter B
 *   CH2 = Exciter A          CH3 = Sensor 1 (S1)
 * S1 is connected via an external cable on this bench; S2 is not.
 *
 * Physical model: two sensor heads share a common antiphase excitation
 * pair (A, B ~= -A) driving the outer plates of a differential
 * capacitor; each head's center (pendulum) plate floats at the
 * capacitive-divider potential S = x*A + (1-x)*B, where
 * x = C_A/(C_A+C_B) = (d0+delta)/(2*d0) for a parallel-plate capacitor
 * at neutral gap d0 and displacement delta. Solving for delta from the
 * measured ADC-domain phasors (see the .c file's top comment for the
 * full derivation, including how the S-channel gain and A/B attenuation
 * combine into k = atten*gain):
 *   x = (S/k - B) / (A - B)
 *   delta = 2*d0*(x - 0.5) - zero_offset
 * Does NOT convert delta to inclination angle -- that needs empirical
 * pendulum/flexure calibration and is later work.
 *
 * Same ISR/task-context split WP8 established, but running every single
 * carrier cycle (~2.6 kHz) instead of batched: the per-sample callback
 * (on_sample(), which runs inside Drivers_App/drv_ads131m04.c's SysTick
 * frame drain, not a raw ISR) does cheap per-sample integer I/Q
 * accumulation and pushes one raw snapshot per completed cycle into a
 * ring buffer; svc_displacement_update() (task context, called every
 * scheduler tick) folds config.h's DISPLACEMENT_BATCH_CYCLES consecutive
 * cycles into one coherent I/Q sum before running the float phasor/
 * complex-division/delta math once per batch there -- not once per raw
 * cycle; see config.h's DISPLACEMENT_BATCH_CYCLES comment for why
 * (root-caused 2026-09-24: the division-heavy math alone can't sustain
 * 2.6 kHz once its result is stored anywhere, and doing so unbatched
 * livelocked the whole scheduler). Each batch's result is pushed into a
 * second, output ring buffer -- the per-batch delta1/delta2 stream this
 * module retains (not just a filtered/averaged single value), for later
 * analysis of higher-frequency effects such as pendulum swinging.
 * The svc_displacement_get_*() getters below return the latest batch's
 * snapshot, for the API v2 Measurements (0x4) GET/SUBSCRIBE resources
 * (Services/svc_api.c); svc_displacement_pop() is how a caller gets the
 * full stream instead -- nothing drains it yet on this REV B port's
 * first pass. */

typedef struct {
    float    delta1_mm;    /* S1 displacement, mm -- one DISPLACEMENT_BATCH_CYCLES batch */
    float    residual1;    /* Im(x1) -- diagnostic, should sit near 0 */
    float    delta2_mm;    /* S2 displacement, mm */
    float    residual2;    /* Im(x2) */
    uint16_t seq;           /* Rolling per-CYCLE counter (not per-batch) --
                              * assigned in on_sample() to every completed
                              * 8-sample cycle, including ones later dropped
                              * by a full input ring or a bounded-drain
                              * cap -- and copied here as the LAST raw
                              * cycle folded into this batch, so a consumer
                              * that isn't draining every single entry can
                              * still detect gaps (missing cycles between
                              * consecutive batches' seq values) by their
                              * absence from the sequence. Wraps every
                              * 65536 cycles (~25 s at ~2.6 kHz) --
                              * consumers doing gap detection MUST use
                              * wraparound-safe (modular) comparison, not
                              * naive equality/increment checks, or they'll
                              * see a false gap at every rollover.
                              * Deliberately last/uint16_t rather than
                              * packed in front of the floats: this struct
                              * is read/written by plain field access (the
                              * ring buffer), not memcpy'd onto the wire
                              * directly -- Cortex-M0+ doesn't reliably
                              * support unaligned float access, so this
                              * stays naturally aligned. */
} DisplacementCycle;

/* The 8 raw batch-summed phasors feeding process_one_batch()'s complex
 * division -- one step upstream of delta_mm/residual, exposed 2026-09-25
 * for bench diagnosis (see Config/config.h's "Displacement phasor
 * diagnostics" comment). Field order matches process_one_batch()'s
 * BatchSums exactly (Services/svc_displacement.c) to avoid a
 * transposition hazard when copying between them: B = Exciter B (CH1),
 * A = Exciter A (CH2), S1/S2 = the two sensor heads (CH3/CH0). Plain
 * float, not the int64 the ISR-side accumulator actually holds --
 * process_one_batch() already does this exact int64->float conversion
 * for its own math, so reusing it here costs nothing extra and matches
 * the precision the real demod itself accepts. */
typedef struct {
    float iB, qB;
    float iA, qA;
    float iS1, qS1;
    float iS2, qS2;
} DisplacementPhasors;

/* Registers the ADC sample callback and configures drv_ads131m04.c (but
 * does not start the acquisition trigger -- see svc_displacement_start()
 * below, same split as WP8's svc_signal_analysis.c had). Call once from
 * main.c, checking the return value (CLAUDE.md 7.6). */
DrvStatus svc_displacement_init(void);

/* Start / stop the ADC sample stream + per-cycle demodulation. start()
 * also clears any half-accumulated cycle and both ring buffers so the
 * first results after a start are clean, not a stale mix from before a
 * stop. Idempotent; task context only. Toggled at runtime over the API
 * (EXECUTE / Commands / API2_RES_CMD_DISPLACEMENT). */
DrvStatus svc_displacement_start(void);
void      svc_displacement_stop(void);
bool      svc_displacement_is_running(void);

/* Drains every carrier cycle queued since the last call (the raw-I/Q
 * ISR-adjacent ring buffer), computing x1/x2/delta1/delta2 for each and
 * pushing the result into the output ring buffer svc_displacement_pop()
 * reads, and into this module's own latest-value snapshot (the
 * svc_displacement_get_*() getters below). Call every scheduler tick --
 * see the .c file for why this can't wait for a slower task period the
 * way WP9's BME280 task does. */
void svc_displacement_update(void);

/* Latest completed cycle's snapshot -- see this file's top comment for
 * why these live here rather than in g_system_state. Valid only while
 * svc_displacement_get_ok() is true (mirrors g_system_state.ads_ok:
 * acquisition running + at least one cycle processed); all return 0.0f
 * (false, for _ok) before that. Task context only. */
float svc_displacement_get_delta1_mm(void);
float svc_displacement_get_residual1(void);
float svc_displacement_get_delta2_mm(void);
float svc_displacement_get_residual2(void);
bool  svc_displacement_get_ok(void);

/* Latest completed batch's raw phasors -- same validity contract as the
 * getters above (all-zero before the first batch / while !get_ok()).
 * Feeds the API v2 Topic groups (0x5) real-time diagnostic resource
 * (Services/svc_api.c). */
void svc_displacement_get_phasors(DisplacementPhasors *out);

/* Call alongside svc_displacement_update() from the scheduler. If the
 * acquisition driver (Drivers_App/drv_ads131m04.c) has latched an
 * integrity fault (lost/duplicated conversion, ring overrun, or lost
 * framing), emits one API2_LOG_ERROR to the debug-log stream and stops
 * the pipeline. One-shot per start() -- ported unchanged from WP8's
 * svc_signal_analysis.c, which this module replaces as the driver's
 * sole sample-callback consumer. */
void svc_displacement_check_integrity(void);

/* Pops the oldest not-yet-read cycle result into *out. Returns false
 * (out untouched) if the output ring buffer is empty -- callers should
 * loop this until it returns false to drain everything available, same
 * pattern as HAL_App/hal_uart.c's hal_uart_read_byte(). If nothing has
 * called this in a while, the buffer overwrites its oldest entries
 * rather than discarding new ones (see the .c file's push_output()
 * comment) -- it always holds the most recent ~24.6 ms of results, not a
 * permanent snapshot of whatever happened to be produced first after a
 * start(). Nothing drains this yet on the REV B port's first pass (the
 * high-rate stream is deferred) -- reserved for that follow-up. */
bool svc_displacement_pop(DisplacementCycle *out);

/* Saturating counts (CLAUDE.md 7.6) -- input_drop: a completed carrier
 * cycle was dropped because the raw-I/Q ring buffer was full (consumer
 * not keeping up). output_drop: a computed result was dropped because
 * the output ring buffer was full (nothing has called
 * svc_displacement_pop() in a while -- expected on this port until a
 * stream consumer exists). degenerate: a cycle's complex division had an
 * exactly-zero denominator (A and B phasors identical) and was skipped
 * entirely -- should not occur in practice. Surfaced over the API on
 * Raw data (0x7) GET API2_RES_RAW_ADC_DIAG. */
uint16_t svc_displacement_get_input_drop_count(void);
uint16_t svc_displacement_get_output_drop_count(void);
uint16_t svc_displacement_get_degenerate_count(void);

/* --- Bulk raw-ADC capture (feeds the API v2 category 0x8 bulk transfer) ---
 * Restored 2026-09-25 -- retiring this alongside the WP8 signal-analysis
 * module it was ported from was a mistake; it's an important bench
 * diagnostic tool independent of the demod math. capture_begin() arms a
 * one-shot fill of an internal Config/config.h ADC_BULK_SAMPLE_COUNT x 4
 * channel buffer with raw sign-extended 24-bit ADC codes (NOT the
 * phasor-accumulator inputs the demod uses), and starts the sample
 * stream. While a capture is armed, on_sample() does only the buffer
 * store -- the phasor accumulation is skipped, so it stays cheap enough
 * not to disturb the scheduler for the ~0.3 s the capture lasts.
 * capture_done() goes true once the buffer is full; capture_end()
 * disarms and stops the stream. Not for concurrent use with the real-time
 * _start()/_stop() path -- the caller (svc_api's bulk dispatch) enforces
 * the exclusivity via svc_displacement_is_running(). Task context only. */
DrvStatus      svc_displacement_capture_begin(void);
bool           svc_displacement_capture_done(void);
void           svc_displacement_capture_end(void);
const uint8_t *svc_displacement_capture_buffer(void);   /* count * ADC_BULK_BYTES_PER_SAMPLE bytes: per sample, ch0..ch3 as 3-byte LE signed */
uint16_t       svc_displacement_capture_sample_count(void);
uint16_t       svc_displacement_capture_drops(void);    /* acquisition ring overflows during the fill (drain fell a ring behind) */

/* Stats of the most recently finished capture, kept past capture_end():
 * samples actually stored, ring-overflow drops, and wall-clock fill time.
 * Effective sample rate = samples * 1000 / elapsed_ms. Any arg may be
 * NULL. All zero until the first capture completes. */
void svc_displacement_last_capture(uint16_t *samples, uint16_t *drops,
                                    uint32_t *elapsed_ms);

/* --- Bulk phasor log capture (feeds the API v2 category 0x8 bulk
 * transfer, resource API2_RES_BULK_PHASORS) ---
 * Added 2026-09-25 for bench diagnosis of behaviour on timescales the
 * ~0.3 s raw-ADC capture above can't reach (drift, degenerate-denominator
 * excursions, a pendulum swinging) -- see Config/config.h's "Displacement
 * phasor diagnostics" comment for the full reasoning. Unlike the raw-ADC
 * capture, this does NOT bypass the phasor accumulation -- it reuses the
 * exact same on_sample()/batching pipeline as normal operation and only
 * changes what svc_displacement_update() does once a batch completes:
 * store a decimated (Config/config.h DISPLACEMENT_PHASOR_LOG_DECIMATION)
 * snapshot here instead of running process_one_batch()'s delta/residual
 * math. Same one-shot arm/fill/drain shape and the same "not for
 * concurrent use with the real-time _start()/_stop() path, caller
 * enforces exclusivity" contract as the raw-ADC capture. Task context
 * only. */
typedef struct {
    float    iB, qB;
    float    iA, qA;
    float    iS1, qS1;
    float    iS2, qS2;
    uint16_t seq;   /* the last raw cycle folded into this stored batch --
                       * same wraparound-safe-comparison caveat as
                       * DisplacementCycle::seq above. */
} __attribute__((packed)) DisplacementPhasorLogEntry;

DrvStatus                         svc_displacement_phasor_log_begin(void);
bool                               svc_displacement_phasor_log_done(void);
void                               svc_displacement_phasor_log_end(void);
const DisplacementPhasorLogEntry *svc_displacement_phasor_log_buffer(void);
uint16_t                          svc_displacement_phasor_log_count(void);   /* always DISPLACEMENT_PHASOR_LOG_DEPTH */

/* Entries stored so far in the current (or most recently finished)
 * capture, 0..DISPLACEMENT_PHASOR_LOG_DEPTH -- lets a host poll progress
 * instead of guessing how long a capture has left. Surfaced over the API
 * on Raw data (0x7) GET API2_RES_RAW_DISPLACEMENT_DIAG. */
uint16_t svc_displacement_phasor_log_progress(void);

#endif /* SVC_DISPLACEMENT_H */
