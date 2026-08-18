#ifndef SVC_DISPLACEMENT_H
#define SVC_DISPLACEMENT_H

#include <stdint.h>
#include <stdbool.h>
#include "drv_common.h"

/* Differential-capacitor displacement sensors S1/S2 (WP10). Replaces
 * Services/svc_signal_analysis.c (WP8's "first cut... additional math
 * may follow" generic 4-channel amplitude/phase diagnostic) -- this is
 * that follow-up, and is now the sole consumer of
 * Drivers_App/drv_ads131m04.c's per-sample callback.
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
 * carrier cycle (~2.6 kHz) instead of batched: the ISR (on_sample) does
 * cheap per-sample integer I/Q accumulation and pushes one raw snapshot
 * per completed cycle into a ring buffer; svc_displacement_update()
 * (task context, called every scheduler tick) drains it and does the
 * float phasor/complex-division/delta math there, pushing each result
 * into a second, output ring buffer -- the per-cycle delta1/delta2
 * stream this module is required to retain (not just a filtered/
 * averaged value), for later analysis of higher-frequency effects such
 * as pendulum swinging. g_system_state.disp1_delta_mm/disp2_delta_mm
 * only hold the latest cycle's snapshot, for a quick "current value"
 * read -- svc_displacement_pop() below is how a caller gets the full
 * stream. */

typedef struct {
    float delta1_mm;    /* S1 displacement, mm */
    float residual1;    /* Im(x1) -- diagnostic, should sit near 0 */
    float delta2_mm;    /* S2 displacement, mm */
    float residual2;    /* Im(x2) */
} DisplacementCycle;

/* Registers the ADC sample callback and starts drv_ads131m04.c -- call
 * once from main.c, checking the return value (CLAUDE.md 7.6, same as
 * svc_signal_analysis_init() before it). */
DrvStatus svc_displacement_init(void);

/* Drains every carrier cycle queued since the last call (the raw-I/Q
 * ISR ring buffer), computing x1/x2/delta1/delta2 for each and pushing
 * the result into the output ring buffer svc_displacement_pop() reads.
 * Call every scheduler tick -- see the .c file for why this can't wait
 * for a slower task period the way WP9's BME280 task does. */
void svc_displacement_update(void);

/* Pops the oldest not-yet-read cycle result into *out. Returns false
 * (out) if the output ring buffer is empty -- callers should loop this
 * until it returns false to drain everything available, same pattern
 * as HAL_App/hal_uart.c's hal_uart_read_byte(). If nothing has called
 * this in a while, the buffer overwrites its oldest entries rather than
 * discarding new ones (see the .c file's push_output() comment) -- it
 * always holds the most recent ~24.6 ms of results, not a permanent
 * snapshot of whatever happened to be produced first after boot. */
bool svc_displacement_pop(DisplacementCycle *out);

/* Saturating counts (CLAUDE.md 7.6) -- input_drop: a completed carrier
 * cycle was dropped because the raw-I/Q ring buffer was full (consumer
 * not keeping up). output_drop: a computed result was dropped because
 * the output ring buffer was full (nothing has called
 * svc_displacement_pop() in a while). degenerate: a cycle's complex
 * division had an exactly-zero denominator (A and B phasors identical)
 * and was skipped entirely -- should not occur in practice. */
uint16_t svc_displacement_get_input_drop_count(void);
uint16_t svc_displacement_get_output_drop_count(void);
uint16_t svc_displacement_get_degenerate_count(void);

#endif /* SVC_DISPLACEMENT_H */
