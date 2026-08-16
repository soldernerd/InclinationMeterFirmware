#include "svc_battery.h"
#include "hal_adc.h"
#include "hal_gpio.h"
#include "hal_power.h"
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

static battery_state_t s_state           = BATTERY_NORMAL;
static uint16_t         s_vbat_mv         = 0;
static uint8_t          s_soc_pct         = 0;
static bool             s_usb_connected   = false;
static bool             s_charging        = false;
static bool             s_charge_complete = false;

/* Shutdown-arm latch — gives the display ~2 s to show a low-battery
 * warning before actually entering low power. Re-checked against USB
 * presence every tick (see svc_battery_update()), not just when armed. */
static bool         s_shutdown_armed   = false;
static uint32_t     s_shutdown_start_ms = 0;

/* Startup grace period: don't trust any battery classification until we've
 * actually seen this many valid ADC scans. s_soc_pct/s_vbat_mv start at 0,
 * which reads as "critical" — without this gate a slow-to-calibrate or
 * momentarily-failing ADC would trip the shutdown latch on a fully charged
 * battery. At the default 100 ms sensor-task period this is ~1 s, well
 * within what's imperceptible to the user at boot. */
#define BATTERY_STARTUP_MIN_SAMPLES  10U
static uint8_t s_valid_sample_count = 0;

static uint16_t adc_to_vbat_mv(uint16_t adc_raw)
{
    /* Vbat_mv = V_ADC_mv × 133 / 33 (100k/33k divider — see pin_config.h).
     * V_ADC_mv comes from the VREFINT-ratiometric conversion, not a raw-
     * code shortcut: REV B ties VREF+ directly to the 3V3_STANDBY rail
     * rather than a fixed-voltage reference, so VDDA can't be assumed
     * constant. */
    uint32_t v_adc_mv = hal_adc_raw_to_mv(adc_raw);
    return (uint16_t)((v_adc_mv * VBAT_SCALE_NUM) / VBAT_SCALE_DEN);
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
    s_state              = BATTERY_NORMAL;
    s_vbat_mv            = 0;
    s_soc_pct            = 0;
    s_usb_connected      = false;
    s_charging           = false;
    s_charge_complete    = false;
    s_shutdown_armed     = false;
    s_valid_sample_count = 0;
}

void svc_battery_enter_low_power(void)
{
    /* Both LEDs and both switched rails off. The MCU's own supply
     * (3V3_STANDBY) is a separate always-on rail, unaffected — this only
     * powers down peripherals. */
    hal_gpio_set(LED_PWR_PORT, LED_PWR_PIN, false);
    hal_gpio_set(LED_STS_PORT, LED_STS_PIN, false);
    hal_gpio_set(PWR_3V3_EN_PORT, PWR_3V3_EN_PIN, true);    /* active-LOW: HIGH = off */
    hal_gpio_set(PWR_5V_EN_PORT, PWR_5V_EN_PIN, false);     /* active-HIGH: LOW = off */

    hal_power_configure_wakeup_pins();
    hal_power_enter_standby();
    /* Unreachable — Standby mode resets the MCU on wake rather than
     * returning here (see hal_power.c). */
}

void svc_battery_update(void)
{
    /* USB / charging / charge-complete detect first — pure GPIO reads,
     * always cheap. */
    s_usb_connected   = hal_gpio_get(VBUS_SENSE_PORT, VBUS_SENSE_PIN);        /* active HIGH */
    s_charging        = !hal_gpio_get(CHARGE_SENSE_PORT, CHARGE_SENSE_PIN);  /* TP4056 CHRG, active LOW */
    s_charge_complete = !hal_gpio_get(STANDBY_SENSE_PORT, STANDBY_SENSE_PIN); /* TP4056 STANDBY, active LOW */

    /* Charge-enable policy: allow charging whenever USB is present, inhibit
     * otherwise. The TP4056 autonomously stops actively charging once full
     * (reflected in s_charge_complete above) — we don't need to toggle CE
     * for that, only for USB presence. */
    hal_gpio_set(CHARGE_EN_PORT, CHARGE_EN_PIN, !s_usb_connected);

    /* Voltage read — only if ADC has produced fresh data this tick */
    const adc_results_t *r = hal_adc_get_results();
    if (r->valid) {
        s_vbat_mv = adc_to_vbat_mv(r->vbat_raw);
        s_soc_pct = vbat_to_soc(s_vbat_mv);
        if (s_valid_sample_count < BATTERY_STARTUP_MIN_SAMPLES) {
            s_valid_sample_count++;
        }
    }

    /* State classification — order matters: charging/full beat low/critical */
    if (s_valid_sample_count < BATTERY_STARTUP_MIN_SAMPLES) {
        /* Not enough confirmed-real samples yet — s_soc_pct/s_vbat_mv may
         * still be zero-initialized defaults, not real data. Assume NORMAL
         * rather than risk tripping the shutdown latch during startup. */
        s_state = BATTERY_NORMAL;
    } else if (s_usb_connected && s_charge_complete) {
        s_state = BATTERY_FULL;
    } else if (s_usb_connected && s_charging) {
        s_state = BATTERY_CHARGING;
    } else if (s_vbat_mv > 0 && s_vbat_mv < g_device_settings.battery_critical_mv) {
        s_state = BATTERY_CRITICAL;
    } else if (s_vbat_mv > 0 && s_vbat_mv < g_device_settings.battery_low_mv) {
        s_state = BATTERY_LOW;
    } else {
        s_state = BATTERY_NORMAL;
    }

    /* Reflect into shared state */
    g_system_state.battery_soc_pct = s_soc_pct;
    g_system_state.battery_charging = s_charging;
    g_system_state.battery_critical = (s_state == BATTERY_CRITICAL);
    g_system_state.usb_connected    = s_usb_connected;

    /* Low-power entry — give the display ~2 s to show a warning first.
     * Only triggers when there's no USB power to charge from; if USB
     * appears at any point (including mid-countdown, including right
     * after waking from a previous low-power entry while still critical)
     * we charge instead of shutting down. */
    if (s_state == BATTERY_CRITICAL && !s_usb_connected) {
        if (!s_shutdown_armed) {
            s_shutdown_armed    = true;
            s_shutdown_start_ms = hal_systick_get_ms();
        } else if ((hal_systick_get_ms() - s_shutdown_start_ms) >= 2000U) {
            svc_battery_enter_low_power();   /* never returns */
        }
    } else {
        s_shutdown_armed = false;
    }
}

battery_state_t svc_battery_get_state(void)      { return s_state; }
uint8_t      svc_battery_get_soc_pct(void)       { return s_soc_pct; }
uint16_t     svc_battery_get_vbat_mv(void)       { return s_vbat_mv; }
bool         svc_battery_is_usb_connected(void)  { return s_usb_connected; }
bool         svc_battery_is_charging(void)       { return s_charging; }
