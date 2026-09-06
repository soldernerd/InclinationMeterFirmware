#ifndef SVC_SIGNAL_ANALYSIS_H
#define SVC_SIGNAL_ANALYSIS_H

#include <stdint.h>
#include <stdbool.h>
#include "drv_common.h"

/* Wires itself to Drivers_App/drv_ads131m04.c's per-sample callback and
 * starts that driver (mirrors Services/svc_ble.c's
 * register-callbacks-then-init-the-driver pattern) -- call once from
 * main.c, checking the return value (CLAUDE.md 7.6 -- drv_ads131m04_init()
 * can fail and the WP7 code review already flagged this exact silent-
 * discard shape once for drv_ad9833_init()). Computes, per ADC channel,
 * the amplitude and phase of the sine wave WP7's AD9833 DAC drives onto
 * it, phase fixed relative to channel 2 (= 0 by definition, per the task
 * spec). See the .c file's top comment for the math this is built on and
 * its current calibration caveats -- explicitly a first cut; "additional
 * math may follow later" per the original request.
 *
 * v0.8.2: _init() only *configures* the ADS131M04 (register writes +
 * MCLK) and registers the per-sample callback — it no longer starts the
 * 20833 Hz sample stream. Nothing consumes the amplitude/phase output
 * yet, and running the stream unconditionally starved the cooperative
 * scheduler's SysTick (erratic status LED). Call
 * svc_signal_analysis_start() to begin sampling; it is toggled at
 * runtime over the API (EXECUTE / COMMANDS / API2_RES_CMD_SIGNAL_ANALYSIS). */
DrvStatus svc_signal_analysis_init(void);

/* Start / stop the ADC sample stream + DFT accumulation. start() also
 * clears any half-accumulated batch so the first finalized result after
 * a start is clean. Idempotent; task context only. */
DrvStatus svc_signal_analysis_start(void);
void      svc_signal_analysis_stop(void);
bool      svc_signal_analysis_is_running(void);

/* --- Bulk raw-ADC capture (feeds the API v2 category 0x8 bulk transfer) ---
 * capture_begin() arms a one-shot fill of an internal
 * Config/config.h ADC_BULK_SAMPLE_COUNT x 4 int32 buffer with raw
 * sign-extended 24-bit ADC codes (NOT the shifted values the DFT uses),
 * and starts the sample stream. While a capture is armed the per-sample
 * ISR does only the buffer store — the DFT MAC is skipped, so the ISR
 * stays cheap enough not to disturb the scheduler for the ~0.2 s the
 * capture lasts. capture_done() goes true once the buffer is full;
 * capture_end() disarms and stops the stream. Not for concurrent use with
 * the real-time _start()/_stop() path — the caller (svc_api bulk
 * dispatch) enforces the exclusivity. Task context only. */
DrvStatus      svc_signal_analysis_capture_begin(void);
bool           svc_signal_analysis_capture_done(void);
void           svc_signal_analysis_capture_end(void);
const int32_t *svc_signal_analysis_capture_buffer(void);   /* [count*4] flattened, ch0..ch3 interleaved */
uint16_t       svc_signal_analysis_capture_sample_count(void);
uint16_t       svc_signal_analysis_capture_drops(void);    /* trigger ticks skipped (DRDY not ready) during the fill */

/* Finalizes the most recently completed accumulation batch, if one is
 * ready (see the .c file for why this is split from the per-sample
 * accumulation instead of doing the float math inline in interrupt
 * context — same "don't do heavy work in an ISR" reasoning as
 * Services/svc_usb.c's rx_handler()). Call periodically from
 * App/app_scheduler.c; a no-op if no batch has completed since the last
 * call. */
void svc_signal_analysis_update(void);

/* Per-channel amplitude (millivolts, peak -- not RMS) and phase
 * (millidegrees, relative to channel 2). Updated once per batch of
 * Config/config.h's SIGNAL_ANALYSIS_BATCH_CYCLES complete sine cycles.
 * chan must be 0-3; out-of-range returns 0 for both. */
int32_t svc_signal_analysis_get_amplitude_mv(uint8_t chan);
int32_t svc_signal_analysis_get_phase_mdeg(uint8_t chan);

#endif /* SVC_SIGNAL_ANALYSIS_H */
