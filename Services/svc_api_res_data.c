#include "svc_api_res.h"
#include "config.h"
#include "system_state.h"
#include "svc_battery.h"
#include "svc_displacement.h"
#include "svc_powertest.h"
#include "drv_ads131m04.h"
#include "hal_power.h"
#include "hal_systick.h"
#include <string.h>

/* API Measurements, Topics, Diagnostics and Procedures: read-only views of live data. */

/* ---------------- Measurements ---------------- */

#define MEAS_VALUE(NAME, CTYPE, EXPR)                                          \
    Api2Status api_h_measurements_##NAME(const ApiResource *r, ApiCall *c)     \
    {                                                                          \
        (void)r;                                                               \
        CTYPE v = (EXPR);                                                      \
        memcpy(c->out, &v, sizeof v);                                          \
        c->out_len = (uint16_t)sizeof v;                                       \
        return API2_STATUS_OK;                                                 \
    }

MEAS_VALUE(onboard_temp,  int16_t,  g_system_state.temperature_cdeg)
MEAS_VALUE(battery_mv,    uint16_t, svc_battery_get_vbat_mv())
MEAS_VALUE(battery_soc,   uint8_t,  svc_battery_get_soc_pct())
MEAS_VALUE(bme_temp,      int16_t,  g_system_state.bme280_temp_cdeg)
MEAS_VALUE(bme_pressure,  uint32_t, g_system_state.bme280_pressure_pa)
MEAS_VALUE(bme_humidity,  uint16_t, g_system_state.bme280_humidity_centipct)
MEAS_VALUE(bme_ok,        uint8_t,  g_system_state.bme280_ok ? 1U : 0U)
MEAS_VALUE(ext_temp,      int16_t,  g_system_state.temp_ext_cdeg)
MEAS_VALUE(ext_temp_ok,   uint8_t,  g_system_state.temp_ext_ok ? 1U : 0U)
MEAS_VALUE(tilt_s1,       float,    svc_displacement_get_delta1_mm())
MEAS_VALUE(tilt_s2,       float,    svc_displacement_get_delta2_mm())
MEAS_VALUE(tilt_diff,     float,    svc_displacement_get_delta_diff_mm())

static uint8_t tilt_flags(void)
{
    return (uint8_t)((svc_displacement_get_display_valid() ? 0x01U : 0U)
                   | (svc_displacement_get_display_doubtful1() ? 0x02U : 0U)
                   | (svc_displacement_get_display_doubtful2() ? 0x04U : 0U));
}

MEAS_VALUE(tilt_flags,    uint8_t,  tilt_flags())
MEAS_VALUE(tilt_seq,      uint16_t, svc_displacement_get_display_seq())

/* ---------------- Topics ---------------- */

Api2Status api_h_topics_env(const ApiResource *r, ApiCall *c)
{
    (void)r;
    Api2TopicsEnvResponse p;
    p.bme280_temp     = g_system_state.bme280_temp_cdeg;
    p.bme280_pressure = g_system_state.bme280_pressure_pa;
    p.bme280_humidity = g_system_state.bme280_humidity_centipct;
    p.bme280_ok       = g_system_state.bme280_ok ? 1U : 0U;
    p.onboard_temp    = g_system_state.temperature_cdeg;
    p.external_temp   = g_system_state.temp_ext_cdeg;
    p.external_temp_ok = g_system_state.temp_ext_ok ? 1U : 0U;
    API2_REPLY(c, &p);
    return API2_STATUS_OK;
}

/* api_h_topics_status lives in svc_api_res_system.c (same payload as System STATE). */

static uint16_t build_live(uint8_t *buf)
{
    Api2TopicsLiveResponse p;
    p.seq       = svc_displacement_get_display_seq();
    p.flags     = tilt_flags();
    p.tilt_s1   = svc_displacement_get_delta1_mm();
    p.tilt_s2   = svc_displacement_get_delta2_mm();
    p.tilt_diff = svc_displacement_get_delta_diff_mm();
    memcpy(buf, &p, sizeof p);
    return (uint16_t)sizeof p;
}

Api2Status api_h_topics_live(const ApiResource *r, ApiCall *c)
{
    (void)r;
    c->out_len = build_live(c->out);
    return API2_STATUS_OK;
}

Api2Status api_h_topics_raw(const ApiResource *r, ApiCall *c)
{
    (void)r;
    Api2TopicsRawResponse p;
    p.tilt_s1       = svc_displacement_get_delta1_mm_raw();
    p.residual_s1   = svc_displacement_get_residual1();
    p.tilt_s2       = svc_displacement_get_delta2_mm_raw();
    p.residual_s2   = svc_displacement_get_residual2();
    p.window_ok_s1  = svc_displacement_get_quality1_ok() ? 1U : 0U;
    p.window_ok_s2  = svc_displacement_get_quality2_ok() ? 1U : 0U;
    p.tilt_diff     = svc_displacement_get_delta_diff_mm_raw();
    p.window_ok_diff = svc_displacement_get_quality_diff_ok() ? 1U : 0U;
    API2_REPLY(c, &p);
    return API2_STATUS_OK;
}

Api2Status api_h_topics_signal_diag(const ApiResource *r, ApiCall *c)
{
    (void)r;
    DisplacementSignalDiag d;
    svc_displacement_get_signal_diag(&d);
    Api2TopicsSignalDiagResponse p;
    p.rms_mv_ch0 = d.rms_mv[0];  p.rms_mv_ch1 = d.rms_mv[1];  p.rms_mv_ch2 = d.rms_mv[2];  p.rms_mv_ch3 = d.rms_mv[3];
    p.p2p_mv_ch0 = d.p2p_mv[0];  p.p2p_mv_ch1 = d.p2p_mv[1];  p.p2p_mv_ch2 = d.p2p_mv[2];  p.p2p_mv_ch3 = d.p2p_mv[3];
    p.phase_deg_ch0 = d.phase_deg[0];  p.phase_deg_ch1 = d.phase_deg[1];
    p.phase_deg_ch2 = d.phase_deg[2];  p.phase_deg_ch3 = d.phase_deg[3];
    p.theoretical_tilt_s1 = d.theoretical_tilt1_mm_per_m;
    p.theoretical_tilt_s2 = d.theoretical_tilt2_mm_per_m;
    API2_REPLY(c, &p);
    return API2_STATUS_OK;
}

/* The LIVE topic pushes once per new display value, and once right away on subscribe. s->a = last pushed seq,
 * s->b = 1 once the first push went out. */
static Api2Status live_start(const ApiResource *r, ApiTransport t, ApiSub *s, const uint8_t *in, uint16_t len)
{
    (void)r; (void)t; (void)in; (void)len;
    if (!s->active) {
        s->a = 0;
        s->b = 0;
    }
    return API2_STATUS_OK;
}

static bool live_poll(const ApiResource *r, ApiTransport t, ApiSub *s, uint8_t *out, uint16_t *len)
{
    (void)r; (void)t;
    uint16_t seq = svc_displacement_get_display_seq();
    if (s->b != 0U && (uint16_t)s->a == seq) {
        return false;
    }
    s->a = seq;
    s->b = 1U;
    *len = build_live(out);
    return true;
}

const ApiEventOps api_ev_live = { live_start, 0, live_poll };

/* Placeholders for resources that are subscription-only (the table needs a handler for every row). */
Api2Status api_h_topics_phasor_stream(const ApiResource *r, ApiCall *c)
{
    (void)r; (void)c;
    return API2_STATUS_VERB_NOT_VALID;
}

/* ---------------- Diagnostics ---------------- */

Api2Status api_h_diagnostics_adc(const ApiResource *r, ApiCall *c)
{
    (void)r;
    const Ads131m04Regs *regs = drv_ads131m04_get_regs();
    const volatile Ads131m04Integrity *ig = drv_ads131m04_get_integrity();
    uint16_t samples = 0, drops = 0;
    uint32_t elapsed = 0;
    svc_displacement_last_capture(&samples, &drops, &elapsed);

    Api2DiagnosticsAdcResponse p;
    p.id = regs->id;  p.status = regs->status;  p.mode = regs->mode;  p.clock = regs->clock;
    p.gain1 = regs->gain1;  p.cfg = regs->cfg;
    p.clock_expected = regs->clock_expected;
    p.regs_read_ok = regs->read_ok ? 1U : 0U;
    p.ads_ok = g_system_state.ads_ok ? 1U : 0U;
    p.last_capture_samples = samples;
    p.last_capture_drops = drops;
    p.last_capture_ms = elapsed;
    p.frames_produced = ig->frames_produced;
    p.frames_drained = ig->frames_drained;
    p.tim7_fires = ig->tim7_fires;
    p.ring_overflow = ig->ring_overflow;
    p.drain_clamped = ig->drain_clamped;
    p.framing_err = ig->framing_err;
    p.crc_err = ig->crc_err;
    p.run_ms = ig->run_ms;
    p.frame_deficit = ig->frame_deficit;
    p.frame_deficit_min = ig->frame_deficit_min;
    p.frame_deficit_max = ig->frame_deficit_max;
    p.drain_clamp_max = ig->drain_clamp_max;
    p.word0_last = ig->word0_last;
    p.crc_rx_last = ig->crc_rx_last;
    p.crc_calc_last = ig->crc_calc_last;
    p.fault_code = ig->fault_code;
    p.now_ms = hal_systick_get_ms();
    API2_REPLY(c, &p);
    return API2_STATUS_OK;
}

Api2Status api_h_diagnostics_power(const ApiResource *r, ApiCall *c)
{
    (void)r;
    Api2DiagnosticsPowerResponse p;
    p.mask  = svc_powertest_mask();
    p.rails = (uint8_t)((hal_power_rail_3v3_on() ? 0x01U : 0U) | (hal_power_rail_5v_on() ? 0x02U : 0U));
    API2_REPLY(c, &p);
    return API2_STATUS_OK;
}

Api2Status api_h_diagnostics_displacement(const ApiResource *r, ApiCall *c)
{
    (void)r;
    uint16_t gap_ms, over_count;
    uint32_t gap_at_ms;
    svc_displacement_get_max_update_gap(&gap_ms, &gap_at_ms, &over_count);
    Api2DiagnosticsDisplacementResponse p;
    p.input_drop           = svc_displacement_get_input_drop_count();
    p.degenerate           = svc_displacement_get_degenerate_count();
    p.clip                 = svc_displacement_get_clip_count();
    p.amplitude_fault      = svc_displacement_get_amplitude_fault_count();
    p.max_update_gap_ms    = gap_ms;
    p.max_gap_at_uptime_ms = gap_at_ms;
    p.gap_over_threshold   = over_count;
    p.running              = svc_displacement_is_running() ? 1U : 0U;
    API2_REPLY(c, &p);
    return API2_STATUS_OK;
}

/* ---------------- Procedures ---------------- */

static uint16_t build_zero_cal(uint8_t *buf)
{
    uint16_t progress, target;
    svc_displacement_zero_cal_progress(&progress, &target);
    Api2ProceduresZeroCalResponse p;
    p.phase       = (uint8_t)svc_displacement_zero_cal_get_phase();
    p.progress    = progress;
    p.target      = target;
    p.sensor_mask = svc_displacement_zero_cal_get_mask();
    memcpy(buf, &p, sizeof p);
    return (uint16_t)sizeof p;
}

static uint16_t build_precision(uint8_t *buf)
{
    uint16_t target, count1, count2, count_diff;
    uint32_t elapsed_ms;
    svc_displacement_precision_progress(&count1, &count2, &count_diff, &target, &elapsed_ms);
    bool failed = false;
    float d1 = 0.0f, d2 = 0.0f, ddiff = 0.0f;
    (void)svc_displacement_precision_get_result(&d1, &d2, &ddiff, &failed);
    Api2ProceduresPrecisionResponse p;
    p.phase      = (uint8_t)svc_displacement_precision_get_phase();
    p.target     = target;
    p.count      = count1;
    p.elapsed_ms = elapsed_ms;
    p.failed     = failed ? 1U : 0U;
    p.tilt_s1    = d1;
    p.tilt_s2    = d2;
    p.tilt_diff  = ddiff;
    p.disturbed  = svc_displacement_precision_get_disturbed() ? 1U : 0U;
    memcpy(buf, &p, sizeof p);
    return (uint16_t)sizeof p;
}

Api2Status api_h_procedures_zero_cal(const ApiResource *r, ApiCall *c)
{
    (void)r;
    c->out_len = build_zero_cal(c->out);
    return API2_STATUS_OK;
}

Api2Status api_h_procedures_precision(const ApiResource *r, ApiCall *c)
{
    (void)r;
    c->out_len = build_precision(c->out);
    return API2_STATUS_OK;
}

/* Procedure status pushes: immediately on subscribe, whenever the phase changes, and at most every 250 ms while the
 * numbers (progress, elapsed time) change. s->a = signature of the last push (phase | progress << 8), s->b = phase + 1
 * of the last push (0 = nothing pushed yet), s->last_push_ms = time of the last push. */
#define PROC_PUSH_MIN_MS 250U

static bool proc_poll(ApiSub *s, uint8_t phase, uint32_t sig, uint8_t *out, uint16_t len)
{
    (void)out; (void)len;
    uint32_t now = hal_systick_get_ms();
    bool first   = (s->b == 0U);
    bool changed_phase = (s->b != (uint32_t)phase + 1U);
    bool changed_sig   = (s->a != sig);
    if (!(first || changed_phase || (changed_sig && (uint32_t)(now - s->last_push_ms) >= PROC_PUSH_MIN_MS))) {
        return false;
    }
    s->a = sig;
    s->b = (uint32_t)phase + 1U;
    s->last_push_ms = now;
    return true;
}

static Api2Status proc_start(const ApiResource *r, ApiTransport t, ApiSub *s, const uint8_t *in, uint16_t len)
{
    (void)r; (void)t; (void)in; (void)len;
    if (!s->active) {
        s->a = 0;
        s->b = 0;
    }
    return API2_STATUS_OK;
}

static bool zero_cal_poll(const ApiResource *r, ApiTransport t, ApiSub *s, uint8_t *out, uint16_t *len)
{
    (void)r; (void)t;
    uint16_t progress, target;
    svc_displacement_zero_cal_progress(&progress, &target);
    uint8_t phase = (uint8_t)svc_displacement_zero_cal_get_phase();
    uint16_t n = build_zero_cal(out);
    if (!proc_poll(s, phase, (uint32_t)phase | ((uint32_t)progress << 8), out, n)) return false;
    *len = n;
    return true;
}

static bool precision_poll(const ApiResource *r, ApiTransport t, ApiSub *s, uint8_t *out, uint16_t *len)
{
    (void)r; (void)t;
    uint16_t target, c1, c2, cd;
    uint32_t el;
    svc_displacement_precision_progress(&c1, &c2, &cd, &target, &el);
    uint8_t phase = (uint8_t)svc_displacement_precision_get_phase();
    uint16_t n = build_precision(out);
    uint32_t sig = (uint32_t)phase | ((uint32_t)c1 << 8) | (svc_displacement_precision_get_disturbed() ? 0x1000000UL : 0UL);
    if (!proc_poll(s, phase, sig, out, n)) return false;
    *len = n;
    return true;
}

const ApiEventOps api_ev_zero_cal  = { proc_start, 0, zero_cal_poll };
const ApiEventOps api_ev_precision = { proc_start, 0, precision_poll };
