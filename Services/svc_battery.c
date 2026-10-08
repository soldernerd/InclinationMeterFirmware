#include "svc_battery.h"
#include "drv_sharp_lcd.h"
#include "hal_adc.h"
#include "hal_gpio.h"
#include "hal_power.h"
#include "hal_systick.h"
#include "svc_storage.h"
#include "svc_log.h"
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

/* Charge-enable latch — see the policy comment in update_charge_enable(). */
static bool             s_charge_enabled  = false;
/* "Full" timer for DeviceSettings.charge_full_timeout_min: running while charging is enabled and the SoC
 * reading is 100 %; s_prev_complete is only for logging the STANDBY edge. */
static bool             s_full_timer_on   = false;
static uint32_t         s_full_since_ms   = 0;
static bool             s_prev_complete   = false;

/* Manual "charge regardless of SOC" override (svc_battery_force_charge()).
 * Self-clears on charge-complete or USB removal — see update_charge_enable(). */
static bool             s_force_charge    = false;

/* Manual "charge regardless of anything else" INHIBIT (svc_battery_
 * set_charge_inhibit(), 2026-09-29) -- see svc_battery.h's comment. Checked
 * first in update_charge_enable(), ahead of both s_force_charge and the
 * SOC-threshold auto-policy. Does NOT self-clear -- stays in effect across
 * USB replug/charge-complete until explicitly cancelled. */
static bool             s_charge_inhibited = false;

/* Shutdown-arm latch — gives the display ~2 s to show a low-battery
 * warning before actually entering low power. Re-checked against USB
 * presence every tick (see update_shutdown_arm()), not just when armed. */
static bool         s_shutdown_armed   = false;
static uint32_t     s_shutdown_start_ms = 0;

/* VBUS_SENSE debounce, falling edge only (2026-09-27) -- user-reported: the
 * device sometimes enters Standby (auto-poweroff or critical-battery) despite
 * USB genuinely being connected the whole time. The known ~70mA excess draw
 * on the 5V rail (memory current-consumption-investigation) is enough that a
 * marginal cable/port could sag VBUS momentarily under a load transient (the
 * AFE/display drawing a spike) and have the raw VBUS_SENSE GPIO -- a plain
 * digital read with zero debounce -- misread "unplugged" for a moment. With
 * NO grace period, that single bad tick was enough to un-suppress both
 * auto-poweroff (Services/svc_power.c) and the critical-battery shutdown
 * timer below, even though USB never actually disconnected.
 *
 * Debounced on the WAY AWAY only, not on arrival: a fresh connection should
 * register immediately (so charging starts promptly, matches
 * BATTERY_CHARGE_MIN_SAMPLES' own "don't delay charge start" reasoning) --
 * it's specifically "USB was here and just blinked" that needs forgiveness.
 * s_usb_connected (used everywhere below, including the shutdown-suppression
 * checks) is what gets the grace period; the RAW reading below still gates
 * whether CHARGE_SENSE/STANDBY_SENSE are trusted, since those really are
 * undriven garbage without real VBUS regardless of how recently it was
 * present (see read_charger_inputs()'s comment). */
#define VBUS_DISCONNECT_GRACE_MS  10000U
static uint32_t s_vbus_absent_since_ms;
static bool     s_vbus_absent_pending;

/* Below this, a 1S-LiPo-powered device would already be in hardware UVLO —
 * a reading this low is an ADC/VREF fault (e.g. the 3V3/VREF rail drooping
 * as an external bench supply is dragged down), not the real pack voltage.
 * Such samples must NOT drive the power-off decision. */
#define BATTERY_VBAT_IMPLAUSIBLE_MV   2500U

/* Consecutive genuinely-critical svc_battery_update() ticks (task_battery_ms
 * apart, 1 s default) required before the 2 s shutdown timer may even arm.
 * Filters single/double bad scans near the low-battery region. */
#define BATTERY_CRITICAL_STREAK_MIN  3U
static uint8_t s_critical_streak = 0;

/* Startup grace period: don't trust any battery classification until we've
 * actually seen this many valid ADC scans. s_soc_pct/s_vbat_mv start at 0,
 * which reads as "critical" — without this gate a slow-to-calibrate or
 * momentarily-failing ADC would trip the shutdown latch on a fully charged
 * battery. NOTE: this gates on svc_battery_update() ticks, which run at
 * task_battery_ms (1000 ms default, config.h) — 10 samples is therefore
 * ~10 s in practice, not the ~1 s a faster task period would give. Still
 * imperceptible as a startup delay; kept conservative (10 confirmed
 * readings) rather than shortened, since this is a safety gate. */
#define BATTERY_STARTUP_MIN_SAMPLES  10U
static uint8_t s_valid_sample_count = 0;

/* Charge-enable needs far less confidence than the shutdown path: two
 * confirmed ADC readings are enough to know we're below the charge-start
 * threshold, and a spurious enable on an already-full pack is harmless
 * (the TP4056 just terminates). Waiting the full BATTERY_STARTUP_MIN_SAMPLES
 * here delayed charge start by ~10 s after a USB wake. */
#define BATTERY_CHARGE_MIN_SAMPLES   2U

static uint16_t adc_to_vbat_mv(uint16_t vbat_raw, uint16_t vrefint_raw)
{
    /* Vbat_mv = V_ADC_mv × vbat_scale_num / vbat_scale_den (R6/R9 divider,
     * pin_config.h's BATTERY_SENSE — 33k/100k as of the 2026-09-24 swap,
     * config.h's DEFAULT_VBAT_SCALE_* comment has the before/after ratios;
     * the scale factor itself is EEPROM-backed, not a #define, per project
     * convention). V_ADC_mv comes from the
     * VREFINT-ratiometric conversion, not a raw-code shortcut: REV B ties
     * VREF+ directly to the 3V3_STANDBY rail rather than a fixed-voltage
     * reference, so VDDA can't be assumed constant. Both raw values must
     * come from the same hal_adc_get_results() snapshot — see hal_adc.h. */
    uint32_t v_adc_mv = hal_adc_raw_to_mv(vbat_raw, vrefint_raw);
    int32_t  mv = (int32_t)((v_adc_mv * g_device_settings.vbat_scale_num)
                            / g_device_settings.vbat_scale_den)
                + g_device_settings.vbat_offset_mv;   /* bench-cal additive term */
    return (uint16_t)(mv < 0 ? 0 : mv);
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

static bool startup_grace_active(void)
{
    return s_valid_sample_count < BATTERY_STARTUP_MIN_SAMPLES;
}

void svc_battery_init(void)
{
    s_state              = BATTERY_NORMAL;
    s_vbat_mv            = 0;
    s_soc_pct            = 0;
    s_usb_connected      = false;
    s_charging           = false;
    s_charge_complete    = false;
    s_charge_enabled     = false;
    s_force_charge       = false;
    s_shutdown_armed     = false;
    s_critical_streak    = 0;
    s_valid_sample_count = 0;
    s_vbus_absent_since_ms = 0;
    s_vbus_absent_pending  = false;
}

void svc_battery_enter_low_power(void)
{
    /* Let any in-flight EEPROM write and/or display flush finish rather
     * than cutting them off mid-transaction (PWR_5V_EN below powers the
     * display's SPI2 bus, so a display flush still in flight would get an
     * uncontrolled mid-transfer power loss otherwise) — bounded wait so a
     * genuinely stuck I2C bus or wedged SPI2 can't block shutdown forever;
     * drv_sharp_lcd_update() has its own 50ms give-up on a wedge, well
     * inside this bound. */
    uint32_t wait_start_ms = hal_systick_get_ms();
    while ((svc_storage_is_busy() || drv_sharp_lcd_is_busy()) &&
           hal_systick_elapsed_ms(wait_start_ms) < 250U) {
        svc_storage_update();
        drv_sharp_lcd_update();
    }

    /* Both LEDs and both switched rails off. The MCU's own supply
     * (3V3_STANDBY) is a separate always-on rail, unaffected — this only
     * powers down peripherals. These writes only hold while the GPIO
     * peripheral is actually configured/driving — see
     * hal_power_configure_rail_retention() below for what actually keeps
     * the rails off once Standby mode itself engages. */
    hal_gpio_set(LED_PWR_PORT, LED_PWR_PIN, false);
    hal_gpio_set(LED_STS_PORT, LED_STS_PIN, false);
    hal_gpio_set(PWR_3V3_EN_PORT, PWR_3V3_EN_PIN, true);    /* active-LOW: HIGH = off */
    hal_gpio_set(PWR_5V_EN_PORT, PWR_5V_EN_PIN, false);     /* active-HIGH: LOW = off */

    hal_power_configure_wakeup_pins();
    hal_power_configure_rail_retention();
    hal_power_enter_standby();
    /* Unreachable — Standby mode resets the MCU on wake rather than
     * returning here (see hal_power.c). */
}

/* ---- svc_battery_update()'s sub-steps, in the order they run ---- */

static void read_charger_inputs(void)
{
    bool vbus_raw = hal_gpio_get(VBUS_SENSE_PORT, VBUS_SENSE_PIN);   /* active HIGH */

    /* s_usb_connected gets VBUS_DISCONNECT_GRACE_MS of forgiveness on the
     * way down (see that constant's comment) -- registers a new connection
     * immediately, but only commits to "gone" after it's stayed away
     * continuously for the full grace window. */
    if (vbus_raw) {
        s_usb_connected       = true;
        s_vbus_absent_pending = false;
    } else if (!s_vbus_absent_pending) {
        s_vbus_absent_pending  = true;
        s_vbus_absent_since_ms = hal_systick_get_ms();
    } else if (hal_systick_elapsed_ms(s_vbus_absent_since_ms) >= VBUS_DISCONNECT_GRACE_MS) {
        s_usb_connected = false;
    }

    if (vbus_raw) {
        s_charging        = !hal_gpio_get(CHARGE_SENSE_PORT, CHARGE_SENSE_PIN);   /* TP4056 CHRG, active LOW */
        s_charge_complete = !hal_gpio_get(STANDBY_SENSE_PORT, STANDBY_SENSE_PIN); /* TP4056 STANDBY, active LOW */
    } else {
        /* TP4056 is powered from VBUS — without it these outputs are
         * undriven/meaningless, don't trust whatever they float to. Uses the
         * RAW reading, not the debounced s_usb_connected -- these really are
         * garbage without real VBUS regardless of how recently it was seen. */
        s_charging        = false;
        s_charge_complete = false;
    }
}

static void read_battery_voltage(void)
{
    /* Only if ADC has produced fresh data this tick */
    adc_results_t r = hal_adc_get_results();
    if (r.valid) {
        s_vbat_mv = adc_to_vbat_mv(r.vbat_raw, r.vrefint_raw);
        s_soc_pct = vbat_to_soc(s_vbat_mv);
        if (s_valid_sample_count < BATTERY_STARTUP_MIN_SAMPLES) {
            s_valid_sample_count++;
        }
    }
}

static void update_charge_enable(void)
{
    /* Only start charging once Vbat has actually dropped to
     * battery_charge_start_mv — avoids keeping an already-near-full LiPo
     * topped off, which degrades its life over time. Once started, stay
     * latched on (don't oscillate as Vbat rises back above the threshold
     * mid-charge) until the TP4056 reports complete or USB disappears.
     * s_force_charge (a menu/API override) bypasses the SOC gate but not
     * the USB-present / not-complete guards, and clears itself once
     * either of those goes away — a one-shot "top off now". s_charge_inhibited
     * (2026-09-29) is checked FIRST, ahead of everything else -- a standing
     * "keep it off" instruction that beats both the auto-policy and an
     * armed force_charge, and does NOT self-clear the way force_charge
     * does (svc_battery.h's comment on it has the full reasoning). */
    if (s_charge_inhibited) {
        s_charge_enabled = false;
        s_force_charge   = false;
    } else if (!s_usb_connected || s_charge_complete) {
        s_charge_enabled = false;
        s_force_charge   = false;
    } else if (s_force_charge) {
        s_charge_enabled = true;
    } else if (s_valid_sample_count >= BATTERY_CHARGE_MIN_SAMPLES && s_vbat_mv > 0 &&
               s_vbat_mv < g_device_settings.battery_charge_start_mv) {
        s_charge_enabled = true;
    }

    /* End of charge: the TP4056 STANDBY signal (s_charge_complete, handled above -- it disables charging)
     * or charge_full_timeout_min minutes at a 100 % SoC reading, whichever comes first. The TP4056 cannot
     * terminate by itself while the instrument draws its own current from the battery, so without the timer
     * it floats at ~4.2 V for as long as USB is connected. Stopping clears an armed force-charge; charging
     * restarts through the normal Vbat < battery_charge_start_mv policy above. */
    if (s_charge_complete && !s_prev_complete) {
        svc_log(API2_LOG_INFO, "battery: TP4056 STANDBY - charge complete, charging stopped");
    }
    s_prev_complete = s_charge_complete;
    {
        const uint16_t tmo_min = g_device_settings.charge_full_timeout_min;
        const bool at_full = s_charge_enabled && s_usb_connected && s_soc_pct >= 100U
                             && s_valid_sample_count >= BATTERY_CHARGE_MIN_SAMPLES;
        if (tmo_min != 0U && at_full) {
            if (!s_full_timer_on) {
                s_full_timer_on = true;
                s_full_since_ms = hal_systick_get_ms();
            } else if (hal_systick_elapsed_ms(s_full_since_ms) >= (uint32_t)tmo_min * 60000UL) {
                s_charge_enabled = false;
                s_force_charge   = false;
                s_full_timer_on  = false;
                svc_logf(API2_LOG_INFO, "battery: %u min at 100%% - charging stopped", (unsigned)tmo_min);
            }
        } else {
            s_full_timer_on = false;
        }
    }
    hal_gpio_set(CHARGE_EN_PORT, CHARGE_EN_PIN, !s_charge_enabled);
}

static void classify_battery_state(void)
{
    if (startup_grace_active()) {
        /* Not enough confirmed-real samples yet — s_soc_pct/s_vbat_mv may
         * still be zero-initialized defaults, not real data. Assume NORMAL
         * rather than risk tripping the shutdown latch during startup. */
        s_state = BATTERY_NORMAL;
    } else if (s_usb_connected) {
        /* On external power the pack is being recovered, not run down.
         * Never fall through to the LOW/CRITICAL voltage checks here: a
         * low pack voltage is exactly why USB is plugged in, and
         * update_shutdown_arm() won't power off with USB present anyway,
         * so showing "shutting down" would be wrong. Use s_charge_enabled
         * (our own latch) not just s_charging (the TP4056 CHRG line),
         * which lags the enable by a tick or two. */
        if (s_charge_complete) {
            s_state = BATTERY_FULL;
        } else if (s_charging || s_charge_enabled) {
            s_state = BATTERY_CHARGING;
        } else {
            s_state = BATTERY_NORMAL;   /* USB in, pack healthy, not charging by policy */
        }
    } else if (s_vbat_mv == 0) {
        /* Zero past the startup grace period is a sense-line/ADC fault,
         * not a real reading — treat as critical rather than silently
         * reporting normal. */
        s_state = BATTERY_CRITICAL;
    } else if (s_vbat_mv < g_device_settings.battery_critical_mv) {
        s_state = BATTERY_CRITICAL;
    } else if (s_vbat_mv < g_device_settings.battery_low_mv) {
        s_state = BATTERY_LOW;
    } else {
        s_state = BATTERY_NORMAL;
    }
}

static void update_shutdown_arm(void)
{
    /* Only a *plausible* sub-critical reading counts toward shutdown. A
     * zero / sub-UVLO value is an ADC/VREF fault (classify_battery_state()
     * still flags it CRITICAL for display honesty, but it must not power
     * the device off), and it takes BATTERY_CRITICAL_STREAK_MIN of these
     * in a row before the timer is even allowed to arm. */
    bool real_critical = (s_state == BATTERY_CRITICAL)
                         && (s_vbat_mv >= BATTERY_VBAT_IMPLAUSIBLE_MV)
                         && (s_vbat_mv <  g_device_settings.battery_critical_mv);

    if (real_critical) {
        if (s_critical_streak < 255U) {
            s_critical_streak++;
        }
    } else {
        s_critical_streak = 0;
    }

    /* Give the display ~2 s to show the "Shutting down..." warning first.
     * Only triggers when there's no USB power to charge from; if USB
     * appears at any point (including mid-countdown) we charge instead. */
    if (s_critical_streak >= BATTERY_CRITICAL_STREAK_MIN && !s_usb_connected) {
        if (!s_shutdown_armed) {
            s_shutdown_armed    = true;
            s_shutdown_start_ms = hal_systick_get_ms();
        } else if (hal_systick_elapsed_ms(s_shutdown_start_ms) >= 2000U) {
            svc_battery_enter_low_power();   /* never returns */
        }
    } else {
        s_shutdown_armed = false;
    }
}

void svc_battery_update(void)
{
    read_charger_inputs();
    read_battery_voltage();
    update_charge_enable();
    classify_battery_state();

    /* Granular INFO log on the charger-state edges (debug stream). */
    static bool s_prev_charging = false;
    static bool s_prev_complete = false;
    static bool s_prev_usb      = false;
    if (s_usb_connected != s_prev_usb) {
        svc_log(API2_LOG_INFO, s_usb_connected ? "battery: USB power in" : "battery: USB power out");
        s_prev_usb = s_usb_connected;
    }
    if (s_charging != s_prev_charging) {
        svc_log(API2_LOG_INFO, s_charging ? "battery: charging" : "battery: charge stopped");
        s_prev_charging = s_charging;
    }
    if (s_charge_complete && !s_prev_complete) {
        svc_log(API2_LOG_INFO, "battery: charge complete");
    }
    s_prev_complete = s_charge_complete;

    /* Reflect into shared state */
    g_system_state.battery_soc_pct  = s_soc_pct;
    g_system_state.battery_charging = s_charging;
    g_system_state.battery_low      = (s_state == BATTERY_LOW || s_state == BATTERY_CRITICAL);
    g_system_state.battery_critical = (s_state == BATTERY_CRITICAL);
    g_system_state.usb_connected    = s_usb_connected;

    update_shutdown_arm();
}

battery_state_t svc_battery_get_state(void)      { return s_state; }
uint8_t      svc_battery_get_soc_pct(void)       { return s_soc_pct; }
uint16_t     svc_battery_get_vbat_mv(void)       { return s_vbat_mv; }
bool         svc_battery_is_charging(void)       { return s_charging; }
bool         svc_battery_is_force_charging(void) { return s_force_charge; }

void svc_battery_force_charge(void)
{
    /* Latch the override; update_charge_enable() acts on it next tick
     * (and ignores it if USB isn't actually present). */
    s_force_charge = true;
    svc_log(API2_LOG_INFO, "battery: force-charge armed");
}

void svc_battery_cancel_force_charge(void)
{
    /* Just clear the flag -- update_charge_enable() (called every
     * svc_battery_update() tick) re-evaluates s_charge_enabled from the
     * normal SOC-threshold policy on its own next pass; no need to touch
     * s_charge_enabled or the CHARGE_EN pin directly here. */
    s_force_charge = false;
    svc_log(API2_LOG_INFO, "battery: force-charge cancelled");
}

void svc_battery_set_charge_inhibit(bool inhibit)
{
    /* Just set the flag -- same "let update_charge_enable() re-evaluate
     * next tick" reasoning as cancel_force_charge() above. Logged on both
     * edges (not just "on"), since silently un-inhibiting is exactly the
     * kind of state change worth a log line. */
    s_charge_inhibited = inhibit;
    svc_log(API2_LOG_INFO, inhibit ? "battery: charging inhibited" : "battery: charge inhibit cleared");
}

bool svc_battery_is_charge_inhibited(void) { return s_charge_inhibited; }
