#include "svc_signal_analysis.h"
#include "drv_ads131m04.h"
#include "config.h"
#include <stddef.h>
#include <stdbool.h>
#include <math.h>

/* Single-bin (8-point) DFT at exactly the DAC's own frequency, exploiting
 * WP8's fixed 8-samples-per-cycle relationship (config.h's
 * ADS131M04_OSR_FIELD comment) -- for a pure sine at that exact bin, this
 * is equivalent to (and far cheaper than) a full DFT/FFT:
 *   I = sum(x[n] * cos(2*pi*n/8)), Q = sum(x[n] * sin(2*pi*n/8))
 *   amplitude = (2/N) * sqrt(I^2 + Q^2)
 *   phase     = atan2(Q, I)
 * Channel 2's phase is subtracted from every channel's phase so it reports
 * exactly 0 by construction, per the task spec ("by definition, fix the
 * phase of channel 2 as zero").
 *
 * Split into two phases, same "no heavy work in interrupt context" reasoning
 * Services/svc_usb.c's rx_handler() follows:
 *   - on_sample(), called directly from drv_ads131m04's DMA-completion
 *     interrupt context once per ADC sample (20833.33 Hz) -- pure integer
 *     multiply-accumulate against a fixed Q14 cos/sin table, cheap enough to
 *     run at that rate in an ISR.
 *   - svc_signal_analysis_update(), called from App/app_scheduler.c in
 *     normal task context -- does the float sqrt/atan2 finalization once
 *     per completed batch (~40 Hz), never in interrupt context.
 * The two communicate through a double-buffered snapshot (s_pending_*),
 * written once (as a unit) by on_sample() when a batch completes and read
 * once (as a unit) by svc_signal_analysis_update() -- no partial/torn reads
 * are possible since the ISR never touches s_pending_* again until
 * s_batch_ready has been observed false.
 *
 * Amplitude/phase are recomputed only as often as
 * svc_signal_analysis_update() is called (typically every task_sensors_ms,
 * ~100 ms) even though batches complete faster (~24.6 ms) -- extra
 * completed batches are simply overwritten by the next one rather than
 * queued; fine for a UI-facing readout, same as any other periodically
 * polled sensor value in this codebase.
 *
 * First cut -- "additional math may follow later" per the original
 * request. Not yet calibrated against real hardware. */

#define NUM_CHANNELS       4U
#define SAMPLES_PER_CYCLE  8U
#define REF_SCALE          16384   /* Q14 scale of the cos/sin table below */

/* cos/sin at n*45 degrees (n=0..7), Q14-scaled (x16384). */
static const int32_t s_cos_table[SAMPLES_PER_CYCLE] = {
    16384, 11585, 0, -11585, -16384, -11585, 0, 11585
};
static const int32_t s_sin_table[SAMPLES_PER_CYCLE] = {
    0, 11585, 16384, 11585, 0, -11585, -16384, -11585
};

/* Live accumulators -- touched only from on_sample(), always called from
 * the same interrupt context (see drv_ads131m04.h), so no volatile/locking
 * needed here. */
static uint16_t s_sample_idx  = 0;   /* 0..7, position within current cycle */
static uint16_t s_cycle_count = 0;   /* complete cycles accumulated so far */
static int64_t  s_i_sum[NUM_CHANNELS];
static int64_t  s_q_sum[NUM_CHANNELS];

/* Producer (ISR) -> consumer (scheduler task) handoff -- volatile since
 * both contexts touch these. */
static volatile bool     s_batch_ready = false;
static volatile int64_t  s_pending_i[NUM_CHANNELS];
static volatile int64_t  s_pending_q[NUM_CHANNELS];
static volatile uint32_t s_pending_n;

/* Last-finalized results -- written only by svc_signal_analysis_update()
 * (task context), read only by the getters (also task context). */
static int32_t s_amplitude_mv[NUM_CHANNELS];
static int32_t s_phase_mdeg[NUM_CHANNELS];

static void on_sample(int32_t ch0, int32_t ch1, int32_t ch2, int32_t ch3)
{
    const int32_t ch[NUM_CHANNELS] = { ch0, ch1, ch2, ch3 };
    int32_t c = s_cos_table[s_sample_idx];
    int32_t s = s_sin_table[s_sample_idx];

    for (uint8_t i = 0; i < NUM_CHANNELS; ++i) {
        s_i_sum[i] += (int64_t)ch[i] * c;
        s_q_sum[i] += (int64_t)ch[i] * s;
    }

    s_sample_idx++;
    if (s_sample_idx < SAMPLES_PER_CYCLE) {
        return;
    }
    s_sample_idx = 0;
    s_cycle_count++;
    if (s_cycle_count < SIGNAL_ANALYSIS_BATCH_CYCLES) {
        return;
    }

    /* Batch complete. If the consumer hasn't picked up the previous batch
     * yet, drop this one rather than tear the snapshot -- see file header
     * comment; expected in normal operation since batches complete faster
     * than svc_signal_analysis_update() is called. */
    if (!s_batch_ready) {
        for (uint8_t i = 0; i < NUM_CHANNELS; ++i) {
            s_pending_i[i] = s_i_sum[i];
            s_pending_q[i] = s_q_sum[i];
        }
        s_pending_n   = (uint32_t)SIGNAL_ANALYSIS_BATCH_CYCLES * SAMPLES_PER_CYCLE;
        s_batch_ready = true;
    }

    s_cycle_count = 0;
    for (uint8_t i = 0; i < NUM_CHANNELS; ++i) {
        s_i_sum[i] = 0;
        s_q_sum[i] = 0;
    }
}

DrvStatus svc_signal_analysis_init(void)
{
    s_sample_idx  = 0;
    s_cycle_count = 0;
    s_batch_ready = false;
    for (uint8_t i = 0; i < NUM_CHANNELS; ++i) {
        s_i_sum[i]        = 0;
        s_q_sum[i]        = 0;
        s_amplitude_mv[i] = 0;
        s_phase_mdeg[i]   = 0;
    }

    drv_ads131m04_set_on_sample(on_sample);
    return drv_ads131m04_init();
}

void svc_signal_analysis_update(void)
{
    if (!s_batch_ready) {
        return;
    }

    int64_t  i_sum[NUM_CHANNELS];
    int64_t  q_sum[NUM_CHANNELS];
    uint32_t n = s_pending_n;
    for (uint8_t i = 0; i < NUM_CHANNELS; ++i) {
        i_sum[i] = s_pending_i[i];
        q_sum[i] = s_pending_q[i];
    }
    s_batch_ready = false;   /* consumed -- on_sample() may produce the next batch now */

    float phase_deg[NUM_CHANNELS];
    float amp_raw[NUM_CHANNELS];
    for (uint8_t i = 0; i < NUM_CHANNELS; ++i) {
        /* Divide the reference table's Q14 scale back out before the DFT
         * normalization. */
        float I = (float)i_sum[i] / (float)REF_SCALE;
        float Q = (float)q_sum[i] / (float)REF_SCALE;
        amp_raw[i]   = (2.0f / (float)n) * sqrtf(I * I + Q * Q);
        phase_deg[i] = atan2f(Q, I) * (180.0f / 3.14159265f);
    }

    float phase2 = phase_deg[2];
    for (uint8_t i = 0; i < NUM_CHANNELS; ++i) {
        float rel = phase_deg[i] - phase2;
        while (rel >= 180.0f) { rel -= 360.0f; }
        while (rel < -180.0f) { rel += 360.0f; }
        s_phase_mdeg[i] = (int32_t)(rel * 1000.0f);

        /* Raw ADC code -> mV: 1 LSB = 2.4 V / Gain / 2^24, Gain = 1
         * (datasheet "ADC Conversion Data", also cited in
         * drv_ads131m04.h). amp_raw[i] is peak, not RMS. */
        s_amplitude_mv[i] = (int32_t)(amp_raw[i] * (2400.0f / 16777216.0f));
    }
}

int32_t svc_signal_analysis_get_amplitude_mv(uint8_t chan)
{
    if (chan >= NUM_CHANNELS) {
        return 0;
    }
    return s_amplitude_mv[chan];
}

int32_t svc_signal_analysis_get_phase_mdeg(uint8_t chan)
{
    if (chan >= NUM_CHANNELS) {
        return 0;
    }
    return s_phase_mdeg[chan];
}
