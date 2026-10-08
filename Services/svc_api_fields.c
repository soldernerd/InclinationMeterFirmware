#include "svc_api.h"
#include "svc_api_defs.h"
#include "system_state.h"
#include "svc_storage.h"
#include "svc_log.h"
#include "hal_rtc.h"
#include <string.h>

/* Generic GET / SET of the DeviceSettings members the Calibrations and Settings categories expose. The rows
 * (offset, size, signedness, bounds, hooks) are generated from tools/api_spec.py. SET validates the range and the
 * optional cross-field rule, stores the value, persists the whole settings set to EEPROM and runs the optional hook. */

static int64_t parse_value(const ApiFieldDesc *d, const uint8_t *p)
{
    uint32_t u = 0;
    for (uint8_t i = 0; i < d->size; ++i) {
        u |= (uint32_t)p[i] << (8U * i);
    }
    if (d->is_signed) {
        switch (d->size) {
            case 1:  return (int64_t)(int8_t)u;
            case 2:  return (int64_t)(int16_t)u;
            default: return (int64_t)(int32_t)u;
        }
    }
    return (int64_t)u;
}

Api2Status api_field_handler(const ApiResource *r, ApiCall *c)
{
    const ApiFieldDesc *d = (const ApiFieldDesc *)r->ctx;

    if (c->verb == API2_VERB_GET) {
        memcpy(c->out, (const uint8_t *)&g_device_settings + d->offset, d->size);
        c->out_len = d->size;
        return API2_STATUS_OK;
    }

    /* SET (the core checked the length) */
    if (svc_storage_is_busy()) {
        return API2_STATUS_BUSY_RESOURCE;
    }
    int64_t val = parse_value(d, c->in);
    if (val < d->lo || val > d->hi) {
        return API2_STATUS_INVALID_PARAMETER;
    }
    if (d->check != 0 && !d->check(d->res, val)) {
        return API2_STATUS_INVALID_PARAMETER;
    }

    uint32_t u = (uint32_t)val;
    memcpy((uint8_t *)&g_device_settings + d->offset, &u, d->size);
    svc_storage_validate_settings(&g_device_settings);
    if (svc_storage_save_settings(&g_device_settings) != DRV_OK) {
        g_system_state.settings_save_failed = true;
        svc_logf(API2_LOG_ERROR, "set: res 0x%02X save failed", d->res);
        return API2_STATUS_BUSY_RESOURCE;
    }
    if (d->after != 0) {
        d->after();
    }
    if (d->notify) {
        svc_api_settings_changed();   /* the App re-applies derived state (scheduler periods, ...) */
    }
    svc_logf(API2_LOG_INFO, "%s 0x%02X saved", d->notify ? "setting" : "calibration", d->res);
    return API2_STATUS_OK;
}

/* Cross-field rule: battery_critical_mv must stay below battery_low_mv (svc_battery.c classifies in that order).
 * Per-field bounds cannot express a relation between two resources. */
bool api_check_battery_order(uint8_t res, int64_t value)
{
    if (res == API2_RES_SETTINGS_BATTERY_CRITICAL) {
        return value < (int64_t)g_device_settings.battery_low_mv;
    }
    if (res == API2_RES_SETTINGS_BATTERY_LOW) {
        return value > (int64_t)g_device_settings.battery_critical_mv;
    }
    return true;
}

/* A new clock trim takes effect immediately. */
void api_after_rtc_trim(void)
{
    if (hal_rtc_set_trim_ppm_x10(g_device_settings.rtc_trim_ppm_x10) != DRV_OK) {
        svc_log(API2_LOG_WARN, "rtc: trim not applied");
    }
}
