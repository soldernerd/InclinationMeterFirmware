#ifndef SVC_DISPLACEMENT_INTERNAL_H
#define SVC_DISPLACEMENT_INTERNAL_H

/* Private interface between the pieces of the displacement service -- NOT for use outside
 * Services/svc_displacement*.c / svc_disp_*.c. The public API is svc_displacement.h.
 *
 *   svc_displacement.c         acquisition (sample callback, batch ring), batch demodulation,
 *                              init / start / stop / update, the reading getters, diagnostics
 *   svc_disp_capture.c         the bulk raw-ADC capture buffer (API Bulk)
 *   svc_disp_phasor_stream.c   the gapless phasor FIFO (API Topics 0x05)
 *   svc_disp_procedures.c      the flip (zero) calibration and the precision measurement
 */

#include <stdint.h>
#include <stdbool.h>
#include "math_displacement.h"
#include "math_quality.h"

/* ---- bulk capture (svc_disp_capture.c) ---- */
/* Read by the sample callback on every sample (the hot path): while set, samples go to the
 * capture buffer instead of the phasor accumulators. */
extern volatile bool g_disp_cap_active;
void disp_capture_store(int32_t ch0, int32_t ch1, int32_t ch2, int32_t ch3);

/* ---- phasor stream (svc_disp_phasor_stream.c) ---- */
/* True while a host has the stream open; task context only. */
extern bool g_disp_pstream_active;
/* Queues one completed batch. A full FIFO drops the NEWEST batch (counted). */
void disp_pstream_store(const MathBatchSums *s, uint16_t seq);
void disp_pstream_clear_drops(void);

/* ---- procedures (svc_disp_procedures.c) ---- */
void disp_procs_init(void);                    /* at init: idle, empty */
void disp_procs_break(void);                   /* the batch series was interrupted (a drop, a restart) */
/* One batch finished: delta1/delta2 are the native-sign readings; ds is the display stream, already fed. */
void disp_procs_batch(const MathDisplay *ds, float delta1_mm, float delta2_mm);
void disp_procs_poll(void);                    /* every update tick: ends a timed-out precision measurement */

/* Sign convention is arbitrary at the sensor level -- disp_s1/s2_invert
 * (system_state.h) let each instrument's FINAL reported reading be flipped
 * so "front up" can read positive, without touching the gain/zero-cal math
 * upstream of it. Applied here, at the getter layer, deliberately -- every
 * external consumer (API Measurements/Topics, the LIVE screen, the
 * triggered precision measurement's result) reads through one of these
 * getters, so this is the one place that has to know about the flip.
 * zero_cal_accumulate()/precision_batch() upstream in
 * process_one_batch() use the UNFLIPPED delta1/delta2 locals directly, not
 * these getters -- zero-cal and the precision-run's live progress stay
 * entirely in the sensor's native sign convention; only the finished,
 * reported numbers flip. */
static inline float disp_sensor_sign(uint8_t invert_flag) { return invert_flag ? -1.0f : 1.0f; }

#endif /* SVC_DISPLACEMENT_INTERNAL_H */
