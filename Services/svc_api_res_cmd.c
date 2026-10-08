#include "svc_api_res.h"
#include "config.h"
#include "system_state.h"
#include "svc_battery.h"
#include "svc_displacement.h"
#include "svc_log.h"
#include "svc_power.h"
#include "svc_service.h"
#include "svc_powertest.h"
#include "svc_storage.h"
#include "drv_buzzer.h"
#include "hal_dfu.h"
#include "hal_fault.h"
#include "hal_pintest.h"
#include "hal_power.h"
#include "hal_rtc.h"
#include <string.h>

/* API Commands category: one-shot actions. Handlers that end the session (power off, reboot) answer first via
 * c->after_reply. */

static void let_frame_drain(void)
{
    for (volatile uint32_t i = 0; i < 400000U; ++i) { }
}

static void after_power_off(void)  { let_frame_drain(); svc_power_shutdown_now(); }
static void after_reboot(void)     { let_frame_drain(); hal_power_reset(); }
static void after_reboot_dfu(void) { let_frame_drain(); hal_dfu_enter_bootloader(); }

Api2Status api_h_commands_power_off(const ApiResource *r, ApiCall *c)
{
    (void)r;
    svc_log(API2_LOG_INFO, "cmd: power off");
    c->after_reply = after_power_off;
    return API2_STATUS_OK;
}

Api2Status api_h_commands_reboot(const ApiResource *r, ApiCall *c)
{
    (void)r;
    svc_log(API2_LOG_WARN, "cmd: reboot");
    c->after_reply = after_reboot;
    return API2_STATUS_OK;
}

Api2Status api_h_commands_reboot_dfu(const ApiResource *r, ApiCall *c)
{
    (void)r;
    svc_log(API2_LOG_WARN, "cmd: reboot to DFU (nBOOT0=0; reflash with nBOOT0=1 to recover)");
    c->after_reply = after_reboot_dfu;
    return API2_STATUS_OK;
}

Api2Status api_h_commands_force_charge(const ApiResource *r, ApiCall *c)
{
    (void)r; (void)c;
    svc_battery_force_charge();
    svc_log(API2_LOG_INFO, "cmd: force charge");
    return API2_STATUS_OK;
}

Api2Status api_h_commands_end_charging(const ApiResource *r, ApiCall *c)
{
    (void)r; (void)c;
    svc_battery_cancel_force_charge();
    svc_log(API2_LOG_INFO, "cmd: end charging");
    return API2_STATUS_OK;
}

Api2Status api_h_commands_charge_inhibit(const ApiResource *r, ApiCall *c)
{
    (void)r;
    if (c->in[0] > 1U) {
        return API2_STATUS_INVALID_PARAMETER;
    }
    svc_battery_set_charge_inhibit(c->in[0] != 0U);
    svc_logf(API2_LOG_INFO, "cmd: charge inhibit %s", c->in[0] ? "set" : "cleared");
    return API2_STATUS_OK;
}

Api2Status api_h_commands_displacement(const ApiResource *r, ApiCall *c)
{
    (void)r;
    uint8_t on = c->in[0];
    if (on > 1U) {
        return API2_STATUS_INVALID_PARAMETER;
    }
    /* The ADC serves one consumer: a raw bulk capture and the phasor stream own it while they run. A stop would
     * leave the capture never finishing; a start would look like it worked while the demod stays bypassed. */
    if (api_bulk_active() || svc_displacement_phasor_stream_active()) {
        return API2_STATUS_BUSY_EXCLUSIVE;
    }
    if (on) {
        /* a start while running is a no-op; the only failure is the ADS131M04 not initialised */
        if (svc_displacement_start() != DRV_OK) {
            return API2_STATUS_BUSY_RESOURCE;
        }
    } else {
        svc_displacement_stop();
    }
    svc_logf(API2_LOG_INFO, "cmd: displacement %s", on ? "start" : "stop");
    return API2_STATUS_OK;
}

Api2Status api_h_commands_zero_cal(const ApiResource *r, ApiCall *c)
{
    (void)r;
    /* 1 byte action, optionally (step 1 only) a sensor mask; step 2 continues the sensors chosen in step 1. The
     * core already checked 1 <= length <= 2. */
    uint8_t action = c->in[0];
    uint8_t mask   = (c->in_len >= 2U) ? c->in[1] : ZERO_CAL_SENSORS_BOTH;
    if (action != 1U && c->in_len != 1U) {
        return API2_STATUS_BAD_LENGTH;
    }
    DrvStatus rc;
    switch (action) {
        case 0U:
            svc_displacement_zero_cal_cancel();
            svc_log(API2_LOG_INFO, "cmd: zero-cal cancelled");
            return API2_STATUS_OK;
        case 1U:
            if ((mask & ZERO_CAL_SENSORS_BOTH) == 0U || (mask & (uint8_t)~ZERO_CAL_SENSORS_BOTH) != 0U) {
                return API2_STATUS_INVALID_PARAMETER;
            }
            rc = svc_displacement_zero_cal_step1_begin(mask);
            break;
        case 2U:
            rc = svc_displacement_zero_cal_step2_begin();
            break;
        default:
            return API2_STATUS_INVALID_PARAMETER;
    }
    if (rc != DRV_OK) {
        return API2_STATUS_BUSY_RESOURCE;   /* not running, or the wrong step for the current phase */
    }
    svc_logf(API2_LOG_INFO, "cmd: zero-cal step %u started (sensors 0x%02X)",
             (unsigned)action, (unsigned)svc_displacement_zero_cal_get_mask());
    return API2_STATUS_OK;
}

Api2Status api_h_commands_precision(const ApiResource *r, ApiCall *c)
{
    (void)r;
    switch (c->in[0]) {
        case 0U:
            if (svc_displacement_precision_begin() != DRV_OK) {
                return API2_STATUS_BUSY_RESOURCE;   /* not running, or a zero-cal uses the batch stream */
            }
            svc_log(API2_LOG_INFO, "cmd: precision measurement started");
            return API2_STATUS_OK;
        case 1U:
            svc_displacement_precision_cancel();
            svc_log(API2_LOG_INFO, "cmd: precision measurement cancelled");
            return API2_STATUS_OK;
        default:
            return API2_STATUS_INVALID_PARAMETER;
    }
}

Api2Status api_h_commands_factory_defaults(const ApiResource *r, ApiCall *c)
{
    (void)r;
    if (c->in[0] != 0xA5U) {
        return API2_STATUS_INVALID_PARAMETER;
    }
    if (svc_storage_restore_defaults() != DRV_OK) {
        return API2_STATUS_BUSY_RESOURCE;
    }
    (void)hal_rtc_set_trim_ppm_x10(g_device_settings.rtc_trim_ppm_x10);
    svc_api_settings_changed();
    svc_log(API2_LOG_WARN, "cmd: factory defaults restored (all settings and calibrations)");
    return API2_STATUS_OK;
}

static uint8_t s_fault_kind;

static void after_fault_test(void)
{
    let_frame_drain();
    hal_fault_provoke(s_fault_kind);
}

Api2Status api_h_commands_fault_test(const ApiResource *r, ApiCall *c)
{
    (void)r;
    if (c->in[0] < 1U || c->in[0] > 3U) {
        return API2_STATUS_INVALID_PARAMETER;
    }
    s_fault_kind = c->in[0];
    svc_logf(API2_LOG_ERROR, "cmd: FAULT TEST kind %u -- the instrument will fail now", (unsigned)c->in[0]);
    c->after_reply = after_fault_test;
    return API2_STATUS_OK;
}

Api2Status api_h_commands_service_end(const ApiResource *r, ApiCall *c)
{
    (void)r; (void)c;
    svc_service_leave();
    return API2_STATUS_OK;
}

Api2Status api_h_commands_clear_counters(const ApiResource *r, ApiCall *c)
{
    (void)r; (void)c;
    g_system_state.usb_tx_dropped_count  = 0;
    g_system_state.ble_tx_dropped_count  = 0;
    g_system_state.uart_tx_dropped_count = 0;
    g_system_state.settings_save_failed  = false;
    svc_api_clear_counters();
    svc_displacement_clear_counters();
    svc_log(API2_LOG_INFO, "cmd: counters cleared");
    return API2_STATUS_OK;
}

Api2Status api_h_commands_test_beep(const ApiResource *r, ApiCall *c)
{
    (void)r; (void)c;
    drv_buzzer_beep(BUZZER_TONE_CLICK, 100U);
    svc_log(API2_LOG_INFO, "cmd: test beep");
    return API2_STATUS_OK;
}

static Api2Status reply_mask(ApiCall *c)
{
    Api2CommandsPowerTestResponse p;
    p.applied_mask = svc_powertest_mask();
    API2_REPLY(c, &p);
    return API2_STATUS_OK;
}

Api2Status api_h_commands_power_test(const ApiResource *r, ApiCall *c)
{
    (void)r;
    uint32_t mask = (uint32_t)c->in[0] | ((uint32_t)c->in[1] << 8)
                  | ((uint32_t)c->in[2] << 16) | ((uint32_t)c->in[3] << 24);
    svc_powertest_apply(mask);
    return reply_mask(c);
}

Api2Status api_h_commands_rail(const ApiResource *r, ApiCall *c)
{
    (void)r;
    uint8_t rail = c->in[0], on = c->in[1];
    if (rail > 1U || on > 1U) {
        return API2_STATUS_INVALID_PARAMETER;
    }
    uint32_t bit  = (rail == 0U) ? PWRTEST_3V3_RAIL : PWRTEST_5V_RAIL;
    uint32_t mask = svc_powertest_mask();
    mask = on ? (mask | bit) : (mask & ~bit);
    svc_powertest_apply(mask);
    svc_logf(API2_LOG_WARN, "cmd: rail %s %s", rail ? "5V" : "3V3", on ? "on" : "off");
    return reply_mask(c);
}

Api2Status api_h_commands_pin_test(const ApiResource *r, ApiCall *c)
{
    (void)r;
    uint8_t p = c->in[0];
    if (p & 0x80U) {
        svc_log(API2_LOG_WARN, "pintest: reboot");
        c->after_reply = after_reboot;
        return API2_STATUS_OK;
    }
    hal_pintest_apply(p & 0x3FU, (p & 0x40U) != 0U);
    svc_logf(API2_LOG_WARN, "pintest: pat 0x%02X%s", p & 0x3FU, (p & 0x40U) ? " (DISP_ON allowed)" : "");
    return API2_STATUS_OK;
}

/* Applies a completed flip (zero) calibration (svc_displacement.h DISP_ZERO_CAL_RESULT_READY) to this instrument's
 * own g_device_settings and persists it. svc_displacement.c computes the result but never writes settings itself.
 * Called every tick from api_res_update() so the save happens promptly. */
void api_zero_cal_apply_if_ready(void)
{
    if (svc_displacement_zero_cal_get_phase() != DISP_ZERO_CAL_RESULT_READY) {
        return;
    }
    if (svc_storage_is_busy()) {
        return;   /* a settings write is in flight: leave the result ready and retry next tick */
    }
    float offset1_mm, offset2_mm;
    uint8_t mask;
    if (!svc_displacement_zero_cal_consume_result(&offset1_mm, &offset2_mm, &mask)) {
        return;
    }
    /* The result is the new absolute zero in output units (mm/m). The stored zero is in ppm of the ratio r,
     * independent of k, so a later k calibration cannot invalidate it: zero_ppm = zero * k_micro. A result outside
     * +-DISPLACEMENT_ZERO_PPM_MAX is refused and logged, never clamped and stored: the instrument is far from
     * level, or the two orientations were not a 180 degree flip. */
    float p1 = offset1_mm * (float)g_device_settings.disp_s1_k_micro;
    float p2 = offset2_mm * (float)g_device_settings.disp_s2_k_micro;
    bool in_range = true;
    int32_t off1_ppm = 0, off2_ppm = 0;
    if ((mask & ZERO_CAL_SENSOR_S1) != 0U) {
        if (p1 > (float)DISPLACEMENT_ZERO_PPM_MAX || p1 < -(float)DISPLACEMENT_ZERO_PPM_MAX) {
            in_range = false;
        } else {
            off1_ppm = (int32_t)(p1 + (p1 >= 0.0f ? 0.5f : -0.5f));
        }
    }
    if ((mask & ZERO_CAL_SENSOR_S2) != 0U) {
        if (p2 > (float)DISPLACEMENT_ZERO_PPM_MAX || p2 < -(float)DISPLACEMENT_ZERO_PPM_MAX) {
            in_range = false;
        } else {
            off2_ppm = (int32_t)(p2 + (p2 >= 0.0f ? 0.5f : -0.5f));
        }
    }
    if (!in_range) {
        svc_logf(API2_LOG_ERROR, "zero-cal: result out of range (S1 %ld ppm, S2 %ld ppm, limit +-%ld) -- NOT applied",
                 (long)p1, (long)p2, (long)DISPLACEMENT_ZERO_PPM_MAX);
        return;
    }
    /* only the sensors this run covered are updated; the other keeps its stored zero */
    if (mask & ZERO_CAL_SENSOR_S1) g_device_settings.disp_s1_zero_ppm = off1_ppm;
    if (mask & ZERO_CAL_SENSOR_S2) g_device_settings.disp_s2_zero_ppm = off2_ppm;
    svc_storage_validate_settings(&g_device_settings);
    if (svc_storage_save_settings(&g_device_settings) == DRV_OK) {
        svc_logf(API2_LOG_INFO, "zero-cal: applied (sensors 0x%02X), S1 zero %ld ppm, S2 zero %ld ppm",
                 (unsigned)mask, (long)g_device_settings.disp_s1_zero_ppm, (long)g_device_settings.disp_s2_zero_ppm);
    } else {
        g_system_state.settings_save_failed = true;
        svc_log(API2_LOG_ERROR, "zero-cal: save failed");
    }
}
