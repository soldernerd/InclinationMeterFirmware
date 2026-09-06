#include "svc_measurement.h"
#include "math_settling.h"
#include "hal_systick.h"
#include "system_state.h"
#include "svc_log.h"
#include "config.h"
#include <string.h>

/* Single-shot tilt measurement state machine.
 *
 *   IDLE -> trigger() -> SETTLING (collect samples, run math_settling)
 *   SETTLING -> settled or timeout -> CAPTURING (continue averaging until
 *                                                buffer full)
 *   CAPTURING -> buffer full -> COMPLETE -> notify SINGLE_READY -> wait
 *                                            for acknowledge()
 *   COMPLETE -> acknowledge() -> IDLE
 *
 * Sensors aren't fused/wired into g_system_state's tilt fields yet
 * (WP2/WP5+ territory) — those read as zero here. The packet still
 * carries valid timestamp/battery/status flags so the host can verify
 * the protocol round-trip ahead of real sensor data.
 */

static MeasurementState  s_state = MEAS_STATE_IDLE;
static MeasurementPacket s_packet = {0};

static int32_t   s_buffer[SETTLING_BUFFER_SIZE];
static uint16_t  s_buf_count    = 0;
static uint16_t  s_buf_widx     = 0;

static uint32_t  s_start_ms     = 0;
static uint32_t  s_last_progress_ms = 0;

static uint8_t   build_status_flags(void)
{
    uint8_t f = 0;
    if (g_system_state.sensor_scl3300_ok)   f |= 0x01U;
    if (g_system_state.sensor_pcap04_1_ok)  f |= 0x02U;
    if (g_system_state.sensor_pcap04_2_ok)  f |= 0x04U;
    return f;
}

static int32_t current_sample_umpm(void)
{
    /* Sensors not yet fused into g_system_state — returns whatever's
     * there (currently zero). A future WP feeds real fused tilt here. */
    return g_system_state.tilt_pcap04_umpm;
}

void svc_measurement_init(void)
{
    s_state = MEAS_STATE_IDLE;
    memset(&s_packet, 0, sizeof s_packet);
    math_settling_reset(&s_buf_count, &s_buf_widx);
}

void svc_measurement_trigger(void)
{
    if (s_state != MEAS_STATE_IDLE && s_state != MEAS_STATE_COMPLETE) {
        return;
    }
    s_state             = MEAS_STATE_SETTLING;
    s_start_ms          = hal_systick_get_ms();
    s_last_progress_ms  = s_start_ms;
    math_settling_reset(&s_buf_count, &s_buf_widx);
    svc_log(API2_LOG_INFO, "meas: triggered (settling)");
}

void svc_measurement_cancel(void)
{
    if (s_state != MEAS_STATE_IDLE) {
        svc_log(API2_LOG_INFO, "meas: cancelled");
    }
    s_state = MEAS_STATE_IDLE;
}

MeasurementState         svc_measurement_get_state(void)  { return s_state; }
const MeasurementPacket *svc_measurement_get_packet(void) { return &s_packet; }

void svc_measurement_acknowledge(void)
{
    if (s_state == MEAS_STATE_COMPLETE) {
        s_state = MEAS_STATE_IDLE;
    }
}

uint8_t svc_measurement_get_progress_pct(void)
{
    if (s_state == MEAS_STATE_IDLE)     return 0;
    if (s_state == MEAS_STATE_COMPLETE) return 100;
    /* Linear with sample count — 0% at start, 100% when buffer full. */
    return (uint8_t)((uint32_t)s_buf_count * 100U / (uint32_t)SETTLING_BUFFER_SIZE);
}

static void finalise_packet(int32_t dc_umpm, uint16_t sample_count)
{
    s_packet.tilt_pcap04_umpm    = dc_umpm;
    s_packet.tilt_scl3300_x_umpm = g_system_state.tilt_scl3300_x_umpm;
    s_packet.tilt_scl3300_y_umpm = g_system_state.tilt_scl3300_y_umpm;
    s_packet.temperature_cdeg    = g_system_state.temperature_cdeg;
    s_packet.battery_soc_pct     = g_system_state.battery_soc_pct;
    s_packet.status_flags        = build_status_flags();
    s_packet.timestamp_ms        = hal_systick_get_ms();
    s_packet.sample_count        = sample_count;
}

void svc_measurement_update(void)
{
    if (s_state == MEAS_STATE_IDLE || s_state == MEAS_STATE_COMPLETE) {
        return;
    }

    uint32_t now     = hal_systick_get_ms();
    uint32_t elapsed = now - s_start_ms;

    SettlingResult r;
    SettlingState ss = math_settling_update(
        current_sample_umpm(),
        s_buffer, SETTLING_BUFFER_SIZE,
        &s_buf_count, &s_buf_widx,
        g_device_settings.settling_threshold_umpm,
        g_device_settings.settling_timeout_ms,
        elapsed,
        &r);

    if (s_state == MEAS_STATE_SETTLING) {
        if (ss == SETTLING_SETTLED || ss == SETTLING_TIMEOUT) {
            s_state = MEAS_STATE_CAPTURING;
            svc_log(API2_LOG_INFO, ss == SETTLING_TIMEOUT
                    ? "meas: settling timed out, capturing"
                    : "meas: settled, capturing");
        }
    }

    if (s_state == MEAS_STATE_CAPTURING) {
        if (s_buf_count >= SETTLING_BUFFER_SIZE) {
            finalise_packet(r.dc_umpm, r.sample_count);
            s_state = MEAS_STATE_COMPLETE;
            svc_log(API2_LOG_INFO, "meas: complete");
            /* Completion is picked up by whoever polls
             * svc_measurement_get_state(); API v2 has no single-shot
             * measurement push in the ported subset. */
            return;
        }
    }

    /* Periodic progress notification — every 500 ms during
     * SETTLING/CAPTURING. svc_api owns the actual transmission and reads
     * progress via svc_measurement_get_progress_pct(); this timestamp is
     * currently unused by anything but kept for whoever wires a push
     * path here instead of svc_api's own poll. */
    if ((now - s_last_progress_ms) >= 500U) {
        s_last_progress_ms = now;
    }
}
