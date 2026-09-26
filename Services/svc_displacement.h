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
 * CORRECTED 2026-09-26 (was wrong): both S1 and S2 are connected via an
 * identical 2 m cable and are physically interchangeable -- there is no
 * cabling asymmetry between them. (The two are NOT necessarily in the
 * same rigid housing either -- confirmed 2026-09-26 when the user flipped
 * one 180 degrees independently of the other while investigating a
 * shared drift; see docs/wp10_displacement.md.)
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
 * (false, for _ok) before that. Task context only. delta1/2_mm are the
 * POST-moving-average value (config.h's DISPLACEMENT_MA_SAMPLES, added
 * 2026-09-26) -- see svc_displacement_get_delta1_mm_raw()/
 * get_delta2_mm_raw() below for the pre-MA value. residual1/2 were never
 * run through the MA (zero-cal's own comment explains why) so there is
 * only one version of each. */
float svc_displacement_get_delta1_mm(void);
float svc_displacement_get_residual1(void);
float svc_displacement_get_delta2_mm(void);
float svc_displacement_get_residual2(void);
bool  svc_displacement_get_ok(void);

/* Pre-moving-average delta_mm -- the value computed directly from one
 * DISPLACEMENT_BATCH_CYCLES batch, before DISPLACEMENT_MA_SAMPLES'
 * boxcar smoothing is applied (added 2026-09-26, at the user's request,
 * for granular analysis of the raw ~20.3 updates/s batch stream --
 * Services/svc_api.c's Topic groups (0x5) API2_RES_TOPIC_RAW_DISPLACEMENT
 * exposes this as a subscribable stream). Same validity contract as
 * svc_displacement_get_delta1_mm() above. */
float svc_displacement_get_delta1_mm_raw(void);
float svc_displacement_get_delta2_mm_raw(void);

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

/* --- Clip / amplitude-integrity checks (2026-09-26, added alongside the
 * S1/S2 PGA=16 bump) --- two DIFFERENT things, despite both sounding like
 * "clipping":
 *   clip: a raw ADC code (any of the 4 channels) sits within
 *     ADS131M04_CLIP_THRESHOLD of the ADS131M04's own digital rail
 *     (Drivers_App/drv_ads131m04.h's ADS131M04_CODE_MAX) -- the actual,
 *     direct clipping/saturation detector, checked per raw sample in
 *     on_sample().
 *   amplitude_fault: a completed batch's raw phasor magnitude
 *     (sqrt(i^2+q^2)) exceeds the highest value a genuinely full-scale,
 *     UNDISTORTED sinusoid at the matched carrier frequency could ever
 *     produce over DISPLACEMENT_BATCH_CYCLES cycles (see
 *     process_one_batch()'s comment for the derivation). This is NOT a
 *     second clipping detector -- real analog/ADC clipping flattens the
 *     waveform and only ever REDUCES this value relative to a clean
 *     sinusoid (harmonic energy leaks out of the fundamental bin), so it
 *     can never be triggered by clipping itself. It catches a different
 *     class of problem: a gain/scale mismatch (e.g. the ADC's PGA and the
 *     software `gain` calibration constant disagreeing), corrupted data,
 *     or an accumulation bug -- something that made the computed number
 *     exceed a value that should be mathematically impossible.
 * Both are saturating counts (CLAUDE.md 7.6), reset at start()/init(),
 * logged once (edge-triggered, not every occurrence) via
 * svc_displacement_check_integrity(), and surfaced over the API on Raw
 * data (0x7) GET API2_RES_RAW_DISPLACEMENT_DIAG. Neither stops
 * acquisition -- a signal-quality warning, not an acquisition fault like
 * drv_ads131m04's own integrity checks. */
uint16_t svc_displacement_get_clip_count(void);
uint16_t svc_displacement_get_amplitude_fault_count(void);

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

/* --- Zero calibration (180-degree reversal test) ---
 * Added 2026-09-25 -- see Config/config.h's "Displacement zero
 * calibration" comment for the math. A two-step procedure driven by the
 * API (Commands API2_RES_CMD_ZERO_CAL, Services/svc_api.c):
 *   1. svc_displacement_zero_cal_step1_begin() while the instrument sits
 *      in its starting orientation -- averages DISPLACEMENT_ZERO_CAL_SAMPLES
 *      consecutive batches' delta1_mm/delta2_mm.
 *   2. Once svc_displacement_zero_cal_get_phase() reports
 *      DISP_ZERO_CAL_STEP1_DONE, the instrument is physically rotated
 *      180 degrees and svc_displacement_zero_cal_step2_begin() is called
 *      -- same averaging, at the new orientation.
 *   3. Once the phase reports DISP_ZERO_CAL_RESULT_READY, the new
 *      per-sensor zero offsets are ready; svc_api.c's svc_api_update()
 *      polls for this and applies + persists them to
 *      g_device_settings.disp_s1/s2_zero_offset_um via
 *      svc_displacement_zero_cal_consume_result() (task context, same
 *      "only touch g_device_settings/EEPROM from the API layer" pattern
 *      Calibrations SET already follows) -- this module computes the
 *      result but never writes settings or touches EEPROM itself.
 * Requires svc_displacement_is_running() -- delta1_mm/delta2_mm are only
 * meaningful while the demod is live. Accumulation happens inside
 * process_one_batch() (task context, same place s_delta1_mm etc. get
 * written), so it runs at whatever the current batch rate is -- no
 * separate polling loop needed. svc_displacement_stop() cancels an
 * in-progress run (a stale mid-calibration state after a stop would be
 * more confusing than starting over). */
typedef enum {
    DISP_ZERO_CAL_IDLE = 0,
    DISP_ZERO_CAL_STEP1_RUNNING,
    DISP_ZERO_CAL_STEP1_DONE,        /* ready for step2_begin() after the 180-degree flip */
    DISP_ZERO_CAL_STEP2_RUNNING,
    DISP_ZERO_CAL_RESULT_READY,      /* computed, not yet consumed/applied */
} DisplacementZeroCalPhase;

DrvStatus svc_displacement_zero_cal_step1_begin(void);   /* DRV_ERR_NOT_READY if not running or already in progress */
DrvStatus svc_displacement_zero_cal_step2_begin(void);   /* DRV_ERR_NOT_READY unless phase == DISP_ZERO_CAL_STEP1_DONE */
void      svc_displacement_zero_cal_cancel(void);        /* back to IDLE from any phase; harmless if already idle */

/* count_out and target_out (both may be NULL) report progress within the
 * CURRENT step only (0 while idle or between steps). target_out is
 * always DISPLACEMENT_ZERO_CAL_SAMPLES, included so a host doesn't need
 * to hardcode it. */
DisplacementZeroCalPhase svc_displacement_zero_cal_get_phase(void);
void svc_displacement_zero_cal_progress(uint16_t *count_out, uint16_t *target_out);

/* Only succeeds (returns true, fills offset1_mm_out/offset2_mm_out,
 * resets phase to IDLE) while phase == DISP_ZERO_CAL_RESULT_READY --
 * false (outputs untouched) otherwise. These are the NEW absolute
 * zero-offset values in mm (this run's computed zero error added to
 * whatever disp_s1/s2_zero_offset_um already held, NOT a delta) -- the
 * caller still has to convert to micrometers and persist them; this
 * module never touches g_device_settings or EEPROM itself. */
bool svc_displacement_zero_cal_consume_result(float *offset1_mm_out, float *offset2_mm_out);

/* --- Per-batch quality flag (2026-09-26) --- see Config/config.h's
 * DISPLACEMENT_QUALITY_BAD_MULTIPLE comment for the derivation (bench-
 * validated: a batch's residual step runs ~4-4.6x its typical size at
 * the exact same batches delta_mm has one of the discrete "jumps" this
 * session's noise investigation found). true = this latest batch's
 * delta_mm is trustworthy; false = its residual moved anomalously and
 * the delta_mm from that specific batch should be treated with
 * suspicion. Same validity contract as svc_displacement_get_delta1_mm()
 * (meaningless before the first batch / while !get_ok()). Surfaced over
 * the API on Topic groups (0x5) API2_RES_TOPIC_RAW_DISPLACEMENT, and
 * used internally to gate which batches the precision-measurement
 * feature below averages. */
bool svc_displacement_get_quality1_ok(void);
bool svc_displacement_get_quality2_ok(void);

/* --- Triggered precision measurement (2026-09-26) --- see Config/config.h's
 * DISPLACEMENT_PRECISION_TARGET_SAMPLES comment for the ~2s-vs-64-samples
 * timing tradeoff. API-driven (Commands API2_RES_CMD_PRECISION_MEASURE,
 * Services/svc_api.c): begin() arms averaging of up to
 * DISPLACEMENT_PRECISION_TARGET_SAMPLES quality-good batches PER SENSOR
 * (svc_displacement_get_quality1/2_ok() above), stopping once BOTH
 * sensors reach the target or DISPLACEMENT_PRECISION_TIMEOUT_MS elapses,
 * whichever comes first. Requires the demod already running and no
 * zero-cal in progress (DRV_ERR_NOT_READY otherwise -- the two averaging
 * consumers of the batch stream are mutually exclusive by design, same
 * reasoning as bulk capture vs. real-time demod). A fresh begin() while
 * already running restarts it; svc_displacement_stop() cancels it (same
 * "stale mid-run state is worse than starting over" reasoning zero-cal
 * uses). Unlike zero-cal, the result never touches EEPROM/settings --
 * it's a pure read-back, so there is no separate "consume" step: once
 * DISP_PRECISION_DONE, get_result() can be read repeatedly and stays
 * valid until the next begin(). */
typedef enum {
    DISP_PRECISION_IDLE = 0,
    DISP_PRECISION_RUNNING,
    DISP_PRECISION_DONE,
} DisplacementPrecisionPhase;

DrvStatus svc_displacement_precision_begin(void);    /* DRV_ERR_NOT_READY if not running or zero-cal in progress */
void      svc_displacement_precision_cancel(void);   /* back to IDLE from any phase; harmless if already idle */

DisplacementPrecisionPhase svc_displacement_precision_get_phase(void);

/* All four out-params may be NULL. count1/2_out are batches averaged so
 * far per sensor (0..DISPLACEMENT_PRECISION_TARGET_SAMPLES); target_out
 * is always DISPLACEMENT_PRECISION_TARGET_SAMPLES (so a host doesn't need
 * to hardcode it); elapsed_ms_out is wall-clock time since begin(), 0
 * while idle. */
void svc_displacement_precision_progress(uint16_t *count1_out, uint16_t *count2_out,
                                          uint16_t *target_out, uint32_t *elapsed_ms_out);

/* Only succeeds (returns true) while phase == DISP_PRECISION_DONE --
 * false (outputs untouched) otherwise. delta1/2_mm_out are the mean of
 * whatever quality-good batches were actually collected per sensor
 * (count may be less than the target if timed_out_out is true, or even
 * 0 in a pathological case -- a caller should check
 * svc_displacement_precision_progress()'s counts alongside this to judge
 * confidence, not just trust that the target was met). timed_out_out is
 * true if DISPLACEMENT_PRECISION_TIMEOUT_MS was hit before both sensors
 * reached the target sample count. */
bool svc_displacement_precision_get_result(float *delta1_mm_out, float *delta2_mm_out,
                                            bool *timed_out_out);

#endif /* SVC_DISPLACEMENT_H */
