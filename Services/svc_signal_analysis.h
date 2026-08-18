#ifndef SVC_SIGNAL_ANALYSIS_H
#define SVC_SIGNAL_ANALYSIS_H

#include <stdint.h>
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
 * math may follow later" per the original request. */
DrvStatus svc_signal_analysis_init(void);

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
