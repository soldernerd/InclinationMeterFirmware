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
 * full derivation; superseded -- since 2026-10-06 the calibration is
 * k (empirical sensitivity at PGA 1) and zero, system_state.h):
 *   r = Re[ S/(PGA*D) * e^{-j phase} ],  D = A - B
 *   delta [mm/m] = (r - zero) / k
 * Does NOT convert delta to inclination angle -- that needs empirical
 * pendulum/flexure calibration and is later work.
 *
 * Same ISR/task-context split WP8 established: the per-sample callback
 * (on_sample(), which runs inside Drivers_App/drv_ads131m04.c's SysTick
 * frame drain, not a raw ISR) does one plain int32 add per channel per
 * sample into a sum for that sample's position in the 8-sample carrier
 * cycle, over a whole config.h DISPLACEMENT_BATCH_CYCLES-cycle batch
 * (2026-10-03: this replaced a weighted 64-bit multiply-accumulate per
 * sample -- see config.h's "Per-position batch accumulation" comment), and
 * pushes one entry per completed BATCH into a small ring buffer;
 * svc_displacement_update() (task context, called every scheduler tick)
 * applies the Q14 DFT weights to the position sums once per batch
 * (math_phasor_combine(), exactly the integers the per-sample weighting
 * gave) and then runs the float phasor/complex-division/delta math once
 * per batch there -- not once per raw cycle; see config.h's
 * DISPLACEMENT_BATCH_CYCLES comment for why
 * (root-caused 2026-09-24: the division-heavy math alone can't sustain
 * 2.6 kHz once its result is stored anywhere, and doing so unbatched
 * livelocked the whole scheduler). The svc_displacement_get_*() getters
 * below return the latest values, for the API v2 Measurements (0x4)
 * GET/SUBSCRIBE resources (Services/svc_api.c); the per-batch stream itself
 * is available as the continuous phasor stream (Topic 0x05) and as the raw
 * per-batch delta (Topic 0x03). */

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
 * also clears any half-accumulated batch and the ring so the first results
 * after a start are clean, not a stale mix from before a stop. start() while
 * the acquisition is already running is a no-op (it must not reset state under
 * the live sample callback); it returns DRV_ERR_NOT_READY if the ADC never
 * initialised. Task context only. Toggled at runtime over the API
 * (EXECUTE / Commands / API2_RES_CMD_DISPLACEMENT). */
DrvStatus svc_displacement_start(void);
void      svc_displacement_stop(void);
bool      svc_displacement_is_running(void);

/* Drains the completed batches queued since the last call (the
 * per-position-sum ISR-adjacent ring buffer; at most
 * DISPLACEMENT_MAX_BATCHES_PER_TICK per call), computing
 * x1/x2/delta1/delta2 for each and updating this module's latest-value
 * snapshot (the svc_displacement_get_*() getters below). Call every scheduler tick --
 * see the .c file for why this can't wait for a slower task period the
 * way WP9's BME280 task does. */
void svc_displacement_update(void);

/* Latest completed cycle's snapshot -- see this file's top comment for
 * why these live here rather than in g_system_state. Valid only while
 * svc_displacement_get_ok() is true (mirrors g_system_state.ads_ok:
 * acquisition running + at least one cycle processed); all return 0.0f
 * (false, for _ok) before that. Task context only. delta1/2_mm are the
 * smoothed value: the Hann display stream below once it exists (about 0.6 s
 * after a start), the raw batch value until then (2026-10-07: replaces the
 * 8-batch moving average). See svc_displacement_get_delta1_mm_raw()/
 * get_delta2_mm_raw() below for the per-batch value. residual1/2 are per
 * batch. delta1/2_mm (and _raw) carry the per-instrument
 * sign flip (disp_s1/s2_invert, system_state.h, 2026-09-29) -- residual1/2
 * do not, since they're an internal quality signal, not a directional
 * physical reading. */
float svc_displacement_get_delta1_mm(void);
float svc_displacement_get_residual1(void);
float svc_displacement_get_delta2_mm(void);
float svc_displacement_get_residual2(void);
bool  svc_displacement_get_ok(void);

/* Per-batch delta_mm -- the value computed directly from one
 * DISPLACEMENT_BATCH_CYCLES batch, with no smoothing (added 2026-09-26, at
 * the user's request, for granular analysis of the raw ~40.7 updates/s batch
 * stream --
 * Services/svc_api.c's Topic groups (0x5) API2_RES_TOPIC_RAW_DISPLACEMENT
 * exposes this as a subscribable stream). Same validity contract as
 * svc_displacement_get_delta1_mm() above. */
float svc_displacement_get_delta1_mm_raw(void);
float svc_displacement_get_delta2_mm_raw(void);

/* --- Differential (S1-S2) reading (2026-09-27) --- added once the
 * standard-error analysis in docs/wp10_displacement.md showed a
 * differential reading (one sensor fixed as a reference, the other roved)
 * reaches the target standard error where a single absolute channel
 * cannot, because the dominant noise/drift is common-mode between S1/S2 and
 * cancels in the difference -- the original unit's own two supported
 * configurations (one sensor connected = absolute, both connected =
 * differential) already anticipated this. Just delta1 - delta2 (smoothed or
 * raw, matching the getters above) -- the filters are linear, so no separate
 * state is needed. Same validity contract as svc_displacement_get_delta1_mm()
 * (meaningless before the first batch / while !get_ok()). */
float svc_displacement_get_delta_diff_mm(void);
float svc_displacement_get_delta_diff_mm_raw(void);

/* Display stream (2026-10-05, Hann window 2026-10-07): the same delta
 * readings condensed to about 4 Hz by a Hann window over the newest
 * DISPLACEMENT_DISPLAY_TAPS (25) contiguous batches, published every
 * DISPLACEMENT_DISPLAY_DECIMATION-th batch (config.h has the reasoning).
 * Meant for the local display, not the API. display_seq() increments with
 * every new value (and wraps), so a consumer can redraw exactly when there is
 * something new; display_valid() is false from a (re)start until the first
 * value is ready (about 0.6 s), then true. Per-instrument sign flip applied as
 * for the getters above.
 *
 * display_doubtful1/2() (2026-10-07): the value is shown either way, but this
 * says whether its window looked disturbed -- the mean squared batch-to-batch
 * step of Im(x) over the 25 batches exceeded DISPLACEMENT_QUALITY_K times the
 * instrument's own quiet floor (Math/math_window.h). The LIVE screen puts a "!"
 * next to a doubtful reading. doubtful_diff() = either sensor. Nothing is
 * flagged until the quiet floor rests on two independent 2 s windows (about
 * 4 s after a start). */
float    svc_displacement_get_display_delta1_mm(void);
float    svc_displacement_get_display_delta2_mm(void);
float    svc_displacement_get_display_delta_diff_mm(void);
bool     svc_displacement_get_display_doubtful1(void);
bool     svc_displacement_get_display_doubtful2(void);
bool     svc_displacement_get_display_doubtful_diff(void);
uint16_t svc_displacement_get_display_seq(void);
bool     svc_displacement_get_display_valid(void);

/* Latest completed batch's raw phasors -- same validity contract as the
 * getters above (all-zero before the first batch / while !get_ok()).
 * Feeds the API v2 Topic groups (0x5) real-time diagnostic resource
 * (Services/svc_api.c). */
void svc_displacement_get_phasors(DisplacementPhasors *out);

/* --- Signal diagnostics (2026-09-27) --- amplitude (RMS + peak-to-peak,
 * referred to the ADC pin -- i.e. AFTER each channel's own PGA, so S1/S2
 * already reflect their PGA=16 the same way A/B's PGA=1 needs no further
 * adjustment) and phase for the latest completed batch's 4 raw phasors.
 * Granite-plate calibration tool: the Wyler sensors' own "zero" and "gain"
 * trim pots are adjusted while watching these numbers directly, in real
 * physical units, instead of the derived delta_mm (App/app_display.c's
 * DIAGNOSTICS screen, Services/svc_api.c's Topic groups resource). All
 * amplitudes in millivolts (App/app_display.c picks per-channel display
 * units -- microvolts for S1/S2, millivolts for A/B). theoretical_tilt1/2
 * are the Wyler-handbook-only estimate (config.h's
 * DISPLACEMENT_WYLER_UV_RMS_PER_UM_PER_M: 20uV RMS = 1um/m, applied
 * directly to rms_mv[2]/rms_mv[3] -- S1/S2's own RMS amplitude), computed
 * with NO dependency on atten/gain/d0/cal_mult at all -- an independent
 * cross-check to compare against delta1_mm/delta2_mm side by side. Same
 * validity contract as the other getters (meaningless before the first
 * batch / while !get_ok()). */
typedef struct {
    float rms_mv[4];         /* order: B, A, S1, S2 -- matches DisplacementPhasors */
    float p2p_mv[4];
    float phase_deg[4];      /* atan2(q,i) rotated so that D = A - B sits at 90 deg
                              * (2026-10-06, Sec. 9.1 of docs/signal_processing.tex),
                              * degrees, 0..360: A ~ 90.5, B ~ 269.5, a sensor near
                              * 90 (positive tilt) or 270 (negative). Independent of
                              * the sample-grid alignment. */
    float theoretical_tilt1_mm_per_m;   /* from rms_mv[2] (S1) via the Wyler constant */
    float theoretical_tilt2_mm_per_m;   /* from rms_mv[3] (S2) */
} DisplacementSignalDiag;

void svc_displacement_get_signal_diag(DisplacementSignalDiag *out);

/* Call alongside svc_displacement_update() from the scheduler. If the
 * acquisition driver (Drivers_App/drv_ads131m04.c) has latched an
 * integrity fault (lost/duplicated conversion, ring overrun, or lost
 * framing), emits one API2_LOG_ERROR to the debug-log stream and stops
 * the service (svc_displacement_stop(): the reading is no longer "ok", a
 * running zero-cal / precision measurement is cancelled). One-shot per start() -- ported unchanged from WP8's
 * svc_signal_analysis.c, which this module replaces as the driver's
 * sole sample-callback consumer. */
void svc_displacement_check_integrity(void);

/* Saturating counts (CLAUDE.md 7.6) -- input_drop: carrier cycles lost
 * because the batch ring was full when a batch completed (consumer not
 * keeping up); counted in cycles, DISPLACEMENT_BATCH_CYCLES per dropped
 * whole batch (there are no partial batches). output_drop: always 0 (the
 * output ring was removed 2026-10-07; the wire field is kept). degenerate: a cycle's complex division had an
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

/* --- Scheduler-gap diagnostic (2026-09-26) --- longest gap observed
 * between consecutive svc_displacement_update() calls, i.e. how long the
 * scheduler took to come back around to this task, since the last
 * start(). Added specifically to investigate why observed batch
 * throughput runs below what DISPLACEMENT_BATCH_CYCLES/the ~2.6 kHz
 * production rate alone predicts -- the driver-level acquisition
 * (drv_ads131m04's frame_deficit/ring_overflow) measures perfectly
 * healthy even while this module's own input ring drops, which localizes
 * the cause to scheduler latency, not the ADC/DMA layer; this quantifies
 * it. gap_ms_out saturates at 65535 (any real gap is expected far below
 * that). at_uptime_ms_out is hal_systick_get_ms() at the moment the max
 * was recorded -- lets a host correlate it against a known periodic cost
 * (e.g. does it land near a multiple of the LIVE screen's
 * LIVE_DISPLACEMENT_REFRESH_MS?). over_threshold_count_out is the
 * frequency companion: how many update() calls saw a gap >=
 * DISPLACEMENT_GAP_WARN_THRESHOLD_MS (config.h) since start() -- a rare
 * single huge gap and frequent small-ish ones can produce the same
 * input_drop_count but need completely different fixes, so the max alone
 * doesn't distinguish them. Any output may be NULL. */
void svc_displacement_get_max_update_gap(uint16_t *gap_ms_out, uint32_t *at_uptime_ms_out,
                                          uint16_t *over_threshold_count_out);

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

/* One batch's raw phasors as the continuous phasor stream carries them
 * (API Topic 0x05 / resource 0x05); 34 bytes on the wire. */
typedef struct {
    float    iB, qB;
    float    iA, qA;
    float    iS1, qS1;
    float    iS2, qS2;
    uint16_t seq;   /* the last raw cycle folded into this batch: a rolling
                       * per-cycle counter (wraps every 65536 cycles, ~25 s), so
                       * compare with modular arithmetic. */
} __attribute__((packed)) DisplacementPhasorEntry;

/* --- Continuous phasor batch stream (API Topic 0x5 / resource 0x05) ---
 * Added 2026-10-04 (fw 0.10.65; it replaced the one-shot bulk phasor log,
 * removed 2026-10-07). It delivers EVERY completed batch: svc_displacement_update() pushes each batch's raw phasors into a
 * FIFO (DISPLACEMENT_PHASOR_STREAM_DEPTH entries) and the API layer drains
 * it one frame per batch. The stream is a TAP on the running measurement
 * (changed in fw 0.10.74; before, it replaced the demod and the display
 * showed "not running"): the demod runs as usual and the readings stay
 * valid. begin() starts the measurement if it is not running, end() leaves
 * it running. Refused only while a raw-ADC bulk capture uses the
 * acquisition. Entries carry the cycle seq, so a lost batch shows as a seq
 * jump; batches the FIFO had no room for are counted separately.
 * Task context only. */
/* Investigation hook (fw 0.10.76): set the input multiplexer of the ADC channels in `ch_mask` (bit n = ADC
 * channel n; ch0 = S2, ch1 = B, ch2 = A, ch3 = S1) to `mux` (0 normal, 1 shorted, 2/3 DC test signals), see
 * drv_ads131m04_set_channel_mux(). A running measurement is stopped around the register write and restarted
 * (the demod state restarts, so the readings are invalid for ~2 s afterwards). Not persistent across a reboot. */
DrvStatus svc_displacement_adc_mux(uint8_t ch_mask, uint8_t mux);

DrvStatus svc_displacement_phasor_stream_begin(void);   /* DRV_ERR_NOT_READY if a stream / bulk capture is already active */
void      svc_displacement_phasor_stream_end(void);
bool      svc_displacement_phasor_stream_active(void);
/* Oldest queued entry without removing it (false if empty); consume()
 * drops it once it has been handed to the transport. */
bool      svc_displacement_phasor_stream_peek(DisplacementPhasorEntry *out);
void      svc_displacement_phasor_stream_consume(void);
/* Batches discarded because the FIFO was full, since stream_begin(). */
uint16_t  svc_displacement_phasor_stream_drops(void);

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
 *      g_device_settings.disp_s1/s2_zero_ppm via
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

/* Per-sensor selection (2026-10-06): a calibration run covers the sensors in
 * sensor_mask (bit 0 = S1, bit 1 = S2; ZERO_CAL_SENSORS_BOTH = both); the
 * other sensor keeps its stored zero. Step 2 uses the mask of step 1.
 * DRV_ERR_INVALID if the mask selects no sensor. */
#define ZERO_CAL_SENSOR_S1     0x01U
#define ZERO_CAL_SENSOR_S2     0x02U
#define ZERO_CAL_SENSORS_BOTH  (ZERO_CAL_SENSOR_S1 | ZERO_CAL_SENSOR_S2)
DrvStatus svc_displacement_zero_cal_step1_begin(uint8_t sensor_mask);   /* DRV_ERR_NOT_READY if not running or already in progress */
DrvStatus svc_displacement_zero_cal_step2_begin(void);   /* DRV_ERR_NOT_READY unless phase == DISP_ZERO_CAL_STEP1_DONE */
/* Sensor mask of the current/last run (meaningful while the phase is not IDLE). */
uint8_t   svc_displacement_zero_cal_get_mask(void);
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
 * whatever disp_s1/s2_zero_ppm already held, NOT a delta) -- the
 * caller still has to convert to ppm of the ratio and persist them; this
 * module never touches g_device_settings or EEPROM itself. */
bool svc_displacement_zero_cal_consume_result(float *offset1_mm_out, float *offset2_mm_out,
                                               uint8_t *sensor_mask_out);
/* sensor_mask_out (may be NULL): which sensors the run covered; the caller
 * must only update those sensors' stored zeros. */

/* --- Window-level quality verdict (2026-10-07) --- true = the newest display
 * window (25 batches) was NOT doubtful, i.e. its Im(x) step power was within
 * DISPLACEMENT_QUALITY_K of the instrument's quiet floor (see the display
 * stream above). Surfaced over the API on Topic 0x03 where the per-batch flag
 * used to be (that flag, which compared each batch's residual step with an
 * EWMA of earlier ones, was retired: it fired on the sensor's 20 Hz resonance
 * and dropping the flagged batches made averages worse). Same validity
 * contract as svc_displacement_get_delta1_mm(). The differential is "ok" only
 * when both sensors are. */
bool svc_displacement_get_quality1_ok(void);
bool svc_displacement_get_quality2_ok(void);
bool svc_displacement_get_quality_diff_ok(void);

/* --- Triggered precision measurement (2026-09-26, redesigned 2026-10-07) ---
 * see Config/config.h's "Triggered precision measurement" comment for the
 * reasoning and the data behind it. API-driven (Commands
 * API2_RES_CMD_PRECISION_MEASURE, Services/svc_api.c; right knob on LIVE).
 * begin() starts the clock; after DISPLACEMENT_PRECISION_WINDOW_BATCHES (81
 * = 2 s) contiguous batches every new batch forms a new sliding 81-batch
 * window, and the FIRST window that is clean for BOTH sensors (window-level
 * quality indicator <= DISPLACEMENT_QUALITY_K x the quiet floor, so a
 * disturbance has to have left the window) is the result: the Hann-weighted
 * means of S1, S2 and the differential S1-S2 over that window. If no clean
 * window exists within DISPLACEMENT_PRECISION_TIMEOUT_MS (5 s) the
 * measurement ends in the DONE phase with the failed flag set and no value.
 * A verdict needs the quiet floor (two independent windows, about 4 s after a
 * start of the acquisition): a measurement begun earlier waits for it, which
 * can use up the 5 s. A dropped batch restarts the 2 s fill. Requires the demod already running
 * and no zero-cal in progress (DRV_ERR_NOT_READY otherwise -- the two
 * averaging consumers of the batch stream are mutually exclusive by design).
 * A fresh begin() while already running restarts it; svc_displacement_stop()
 * cancels it. The result never touches EEPROM/settings; once DISP_PRECISION_DONE,
 * get_result() can be read repeatedly and stays valid until the next begin(). */
typedef enum {
    DISP_PRECISION_IDLE = 0,
    DISP_PRECISION_RUNNING,
    DISP_PRECISION_DONE,
} DisplacementPrecisionPhase;

DrvStatus svc_displacement_precision_begin(void);    /* DRV_ERR_NOT_READY if not running or zero-cal in progress */
void      svc_displacement_precision_cancel(void);   /* back to IDLE from any phase; harmless if already idle */

DisplacementPrecisionPhase svc_displacement_precision_get_phase(void);

/* While RUNNING: true if the newest full window was not clean (the measurement
 * is waiting for the disturbance to pass). False otherwise. */
bool svc_displacement_precision_get_disturbed(void);

/* All out-params may be NULL. count1/2/diff_out are all the same number now:
 * contiguous batches since begin(), capped at target_out (the window length,
 * DISPLACEMENT_PRECISION_WINDOW_BATCHES = 81); three fields are kept so the
 * wire format did not change. elapsed_ms_out is wall-clock time since begin(),
 * 0 while idle. */
void svc_displacement_precision_progress(uint16_t *count1_out, uint16_t *count2_out,
                                          uint16_t *count_diff_out, uint16_t *target_out,
                                          uint32_t *elapsed_ms_out);

/* Only succeeds (returns true) while phase == DISP_PRECISION_DONE -- false
 * (outputs untouched) otherwise. failed_out is true if no clean window was
 * found in time: then the three values are 0 and must not be used. Otherwise
 * delta1/2_mm_out are the Hann-weighted window means and delta_diff_mm_out is
 * their difference (= the mean of the per-batch differences). All three carry
 * the per-instrument sign flip (disp_s1/s2_invert); the differential is
 * computed from the flipped values, so it is right for any combination. */
bool svc_displacement_precision_get_result(float *delta1_mm_out, float *delta2_mm_out,
                                            float *delta_diff_mm_out, bool *failed_out);

#endif /* SVC_DISPLACEMENT_H */
