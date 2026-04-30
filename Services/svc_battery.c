#include "svc_battery.h"
#include "hal_adc.h"
#include "hal_gpio.h"
#include "hal_systick.h"
#include "system_state.h"
#include "config.h"
#include "pin_config.h"

/* LiPo OCV → SOC table, 11 points in 10 % steps. Linear interpolation
 * between adjacent points. Conservative — rest voltages, not under-load. */
static const uint16_t s_soc_voltage_mv[] = {
    3000, 3300, 3500, 3600, 3650, 3700, 3750, 3800, 3870, 3980, 4200,
};
#define SOC_TABLE_LEN  ((sizeof s_soc_voltage_mv) / sizeof s_soc_voltage_mv[0])

static BatteryState s_state         = BATTERY_NORMAL;
static uint16_t     s_vbat_mv       = 0;
static uint8_t      s_soc_pct       = 0;
static bool         s_usb_connected = false;
static bool         s_charging      = false;

/* Critical-shutdown latch — once tripped we run the shutdown sequence
 * and never recover. */
static bool         s_shutdown_armed   = false;
static uint32_t     s_shutdown_start_ms = 0;

static uint16_t adc_to_vbat_mv(uint16_t adc_raw)
{
    /* Vbat_mv = adc_raw × 21 / 17 (see pin_config.h) */
    return (uint16_t)(((uint32_t)adc_raw * VBAT_SCALE_NUM) / VBAT_SCALE_DEN);
}

static uint8_t vbat_to_soc(uint16_t vbat_mv)
{
    if (vbat_mv <= s_soc_voltage_mv[0])                return 0;
    if (vbat_mv >= s_soc_voltage_mv[SOC_TABLE_LEN - 1]) return 100;

    for (uint8_t i = 1; i < SOC_TABLE_LEN; ++i) {
        if (vbat_mv < s_soc_voltage_mv[i]) {
            uint16_t v_lo = s_soc_voltage_mv[i - 1];
            uint16_t v_hi = s_soc_voltage_mv[i];
            uint16_t pct_lo = (uint16_t)((i - 1) * 10);
            /* linear interp: pct = pct_lo + (vbat - v_lo) * 10 / (v_hi - v_lo) */
            uint32_t num = (uint32_t)(vbat_mv - v_lo) * 10U;
            uint32_t den = (uint32_t)(v_hi - v_lo);
            return (uint8_t)(pct_lo + (uint8_t)(num / den));
        }
    }
    return 100;
}

void svc_battery_init(void)
{
    s_state          = BATTERY_NORMAL;
    s_vbat_mv        = 0;
    s_soc_pct        = 0;
    s_usb_connected  = false;
    s_charging       = false;
    s_shutdown_armed = false;
}

void svc_battery_update(void)
{
    /* USB / charging detect first — pure GPIO reads, always cheap */
    s_usb_connected = hal_gpio_get(VBUS_SENSE_PORT, VBUS_SENSE_PIN);       /* active HIGH */
    s_charging      = !hal_gpio_get(CHARGE_SENSE_PORT, CHARGE_SENSE_PIN); /* TP4056 active LOW */

    /* Voltage read — only if ADC has produced fresh data this tick */
    const AdcResults *r = hal_adc_get_results();
    if (r->valid) {
        s_vbat_mv = adc_to_vbat_mv(r->vbat_raw);
        s_soc_pct = vbat_to_soc(s_vbat_mv);
    }

    /* State classification — order matters: charging beats low/critical */
    if (s_usb_connected && s_charging) {
        s_state = BATTERY_CHARGING;
    } else if (s_usb_connected && !s_charging && s_soc_pct >= 95U) {
        s_state = BATTERY_FULL;
    } else if (s_vbat_mv > 0 && s_vbat_mv < g_device_settings.battery_cutoff_mv) {
        s_state = BATTERY_CRITICAL;
    } else if (s_soc_pct < g_device_settings.battery_critical_pct) {
        s_state = BATTERY_CRITICAL;
    } else if (s_soc_pct < g_device_settings.battery_low_pct) {
        s_state = BATTERY_LOW;
    } else {
        s_state = BATTERY_NORMAL;
    }

    /* Reflect into shared state */
    g_system_state.battery_soc_pct = s_soc_pct;
    g_system_state.battery_charging = s_charging;
    g_system_state.battery_critical = (s_state == BATTERY_CRITICAL);
    g_system_state.usb_connected    = s_usb_connected;

    /* Shutdown sequence — give the display ~2 s to redraw, then cut the
     * 3.3 V rail. We never recover from this; the latch ensures a single
     * shot rather than repeated retriggering. */
    if (s_state == BATTERY_CRITICAL && !s_shutdown_armed) {
        s_shutdown_armed   = true;
        s_shutdown_start_ms = hal_systick_get_ms();
    }
    if (s_shutdown_armed) {
        if ((hal_systick_get_ms() - s_shutdown_start_ms) >= 2000U) {
            /* !3V3_EN! is active LOW — driving it HIGH cuts the rail. The
             * old active-high LDO_EN cut power on `false`; this is the
             * inverted equivalent, not a straight rename. */
            hal_gpio_set(_3V3_ENABLE__PORT, _3V3_ENABLE__PIN, true);
            /* MCU loses power within microseconds — code below never
             * runs in practice, but spin to avoid undefined fall-through */
            for (;;) { }
        }
    }
}

BatteryState svc_battery_get_state(void)         { return s_state; }
uint8_t      svc_battery_get_soc_pct(void)       { return s_soc_pct; }
uint16_t     svc_battery_get_vbat_mv(void)       { return s_vbat_mv; }
bool         svc_battery_is_usb_connected(void)  { return s_usb_connected; }
bool         svc_battery_is_charging(void)       { return s_charging; }
