#include "svc_api_res.h"
#include "app_version.h"
#include "build_info.h"      /* FW_BUILD_ID, generated at build time */
#include "config.h"
#include "system_state.h"
#include "svc_battery.h"
#include "svc_displacement.h"
#include "svc_log.h"
#include "svc_service.h"
#include "hal_fault.h"
#include "hal_mcu.h"
#include "hal_power.h"
#include "hal_rtc.h"
#include "hal_systick.h"
#include <string.h>

/* API System category: identity, live state, health, real-time clock. */

static void copy_fixed(char *dst, const char *src, size_t cap)
{
    size_t n = 0;
    while (n < cap && src[n] != '\0') { n++; }
    memcpy(dst, src, n);
    if (n < cap) {
        memset(dst + n, 0, cap - n);
    }
}

/* 8 uppercase hex chars, no terminator -- exactly the serial field's width. */
static void format_hex32(char *dst, uint32_t v)
{
    static const char digits[] = "0123456789ABCDEF";
    for (int8_t i = 7; i >= 0; --i) {
        dst[i] = digits[v & 0xFU];
        v >>= 4;
    }
}

Api2Status api_h_system_identity(const ApiResource *r, ApiCall *c)
{
    (void)r;
    Api2SystemIdentityResponse p;
    memset(&p, 0, sizeof p);
    p.fw_major    = (uint8_t)FW_VERSION_MAJOR;
    p.fw_minor    = (uint8_t)FW_VERSION_MINOR;
    p.fw_patch    = (uint8_t)FW_VERSION_PATCH;
    p.api_version = (uint8_t)API2_VERSION;
    p.max_payload = (uint16_t)API2_PACKET_MAX_PAYLOAD;
    copy_fixed(p.product, USB_PRODUCT_STR, sizeof p.product);
    /* all three words of the 96-bit factory UID folded to 32 bits (HAL_App/hal_mcu.c): the same value the STATUS
     * screen shows and the USB serial string is derived from */
    format_hex32(p.serial, hal_mcu_uid_low());
    copy_fixed(p.build, FW_BUILD_ID, sizeof p.build);
    API2_REPLY(c, &p);
    return API2_STATUS_OK;
}

static void fill_state(Api2SystemStateResponse *p)
{
    p->battery_state        = (uint8_t)svc_battery_get_state();
    p->battery_soc_pct      = svc_battery_get_soc_pct();
    p->battery_mv           = svc_battery_get_vbat_mv();
    p->usb_connected        = g_system_state.usb_connected ? 1U : 0U;
    p->ble_connected        = g_system_state.ble_connected ? 1U : 0U;
    p->charging             = svc_battery_is_charging() ? 1U : 0U;
    p->force_charging       = svc_battery_is_force_charging() ? 1U : 0U;
    p->charge_inhibited     = svc_battery_is_charge_inhibited() ? 1U : 0U;
    p->rail_3v3_on          = hal_power_rail_3v3_on() ? 1U : 0U;
    p->rail_5v_on           = hal_power_rail_5v_on() ? 1U : 0U;
    p->displacement_running = svc_displacement_is_running() ? 1U : 0U;
    p->phasor_stream_active = svc_displacement_phasor_stream_active() ? 1U : 0U;
    p->bulk_active          = api_bulk_active() ? 1U : 0U;
    p->service_mode         = svc_service_active() ? 1U : 0U;
}

Api2Status api_h_system_state(const ApiResource *r, ApiCall *c)
{
    (void)r;
    Api2SystemStateResponse p;
    fill_state(&p);
    API2_REPLY(c, &p);
    return API2_STATUS_OK;
}

/* Topics STATUS carries the same payload (the generator checks both layouts are the same size). */
Api2Status api_h_topics_status(const ApiResource *r, ApiCall *c)
{
    _Static_assert(sizeof(Api2TopicsStatusResponse) == sizeof(Api2SystemStateResponse), "STATUS topic == STATE");
    return api_h_system_state(r, c);
}

Api2Status api_h_system_health(const ApiResource *r, ApiCall *c)
{
    (void)r;
    Api2SystemHealthResponse p;
    p.adc_ok               = g_system_state.adc_ok ? 1U : 0U;
    p.dac_ok               = g_system_state.dac_ok ? 1U : 0U;
    p.ads_ok               = g_system_state.ads_ok ? 1U : 0U;
    p.display_ok           = g_system_state.display_ok ? 1U : 0U;
    p.bme280_ok            = g_system_state.bme280_ok ? 1U : 0U;
    p.eeprom_selftest      = g_system_state.eeprom_selftest;
    p.settings_save_failed = g_system_state.settings_save_failed ? 1U : 0U;
    p.woke_from_standby    = g_system_state.woke_from_standby ? 1U : 0U;
    p.reset_cause          = hal_power_reset_cause();
    p.rtc_set              = hal_rtc_is_set() ? 1U : 0U;
    p.uptime_s             = hal_systick_get_ms() / 1000U;
    p.usb_tx_dropped       = g_system_state.usb_tx_dropped_count;
    p.ble_tx_dropped       = g_system_state.ble_tx_dropped_count;
    p.uart_tx_dropped      = g_system_state.uart_tx_dropped_count;
    p.rx_malformed         = svc_api_rx_malformed();
    const MathFaultRecord *f = hal_fault_last();
    p.last_fault_kind      = f->kind;
    p.last_fault_pc        = f->pc;
    p.last_fault_lr        = f->lr;
    API2_REPLY(c, &p);
    return API2_STATUS_OK;
}

Api2Status api_h_system_rtc(const ApiResource *r, ApiCall *c)
{
    (void)r;
    if (c->verb == API2_VERB_GET) {
        rtc_datetime_t dt;
        hal_rtc_get(&dt);
        Api2SystemRtcResponse p;
        p.year      = dt.year;
        p.month     = dt.month;
        p.day       = dt.day;
        p.weekday   = dt.weekday;
        p.hour      = dt.hour;
        p.minute    = dt.minute;
        p.second    = dt.second;
        p.is_set    = hal_rtc_is_set() ? 1U : 0U;
        p.subsecond = dt.subsecond;
        API2_REPLY(c, &p);
        return API2_STATUS_OK;
    }

    /* SET */
    const uint8_t *b = c->in;
    rtc_datetime_t dt = {
        .year   = (uint16_t)(b[0] | ((uint16_t)b[1] << 8)),
        .month  = b[2], .day = b[3], .weekday = 0,
        .hour   = b[4], .minute = b[5], .second = b[6], .subsecond = 0,
    };
    if (!hal_rtc_datetime_valid(&dt)) {
        return API2_STATUS_INVALID_PARAMETER;
    }
    if (hal_rtc_set(&dt) != DRV_OK) {
        return API2_STATUS_BUSY_RESOURCE;
    }
    svc_logf(API2_LOG_INFO, "rtc set %04u-%02u-%02u %02u:%02u:%02u",
             dt.year, dt.month, dt.day, dt.hour, dt.minute, dt.second);
    return API2_STATUS_OK;
}
