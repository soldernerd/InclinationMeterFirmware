/* Host tests for Services/svc_battery.c -- the REAL source is included, the hardware under it (GPIO, ADC, clock, standby)
 * is a set of test doubles. Covers the voltage and state-of-charge conversion, the classification thresholds, the startup
 * grace, the charge-enable policy (latch, force, inhibit), the VBUS debounce and the critical-battery shutdown. */
#include "test.h"
#include <string.h>

#include "../system_state.h"
#include "../Config/config.h"
#include "../Config/pin_config.h"
#include "../HAL_App/hal_adc.h"
#include "../Services/svc_log.h"

/* ---------------- doubles ---------------- */
GPIO_TypeDef g_host_gpio[6];
TIM_TypeDef  g_host_tim[8];
DeviceSettings g_device_settings;
SystemState    g_system_state;

static uint32_t g_ms;
uint32_t hal_systick_get_ms(void) { return g_ms; }
uint32_t hal_systick_elapsed_ms(uint32_t start) { return g_ms - start; }

#define PIN_INDEX(pin) (__builtin_ctz(pin))
static uint8_t g_in[6][16];                 /* input levels by [port][pin number] */
static uint8_t g_out[6][16];                /* last level written */
static int port_idx(GPIO_TypeDef *p) { return (int)(p - g_host_gpio); }
bool hal_gpio_get(GPIO_TypeDef *port, uint16_t pin) { return g_in[port_idx(port)][PIN_INDEX(pin)] != 0; }
void hal_gpio_set(GPIO_TypeDef *port, uint16_t pin, bool v) { g_out[port_idx(port)][PIN_INDEX(pin)] = v ? 1 : 0; }

static adc_results_t g_adc;
adc_results_t hal_adc_get_results(void) { return g_adc; }
uint32_t hal_adc_raw_to_mv(uint16_t raw, uint16_t vref) { (void)vref; return raw; }   /* the "ADC" reads millivolts */

static int g_standby_calls, g_leave_calls, g_logs;
void hal_power_configure_wakeup_pins(void) {}
void hal_power_configure_rail_retention(void) {}
void hal_power_enter_standby(void) { g_standby_calls++; }
bool drv_sharp_lcd_is_busy(void) { return false; }
void drv_sharp_lcd_update(void) {}
bool svc_storage_is_busy(void) { return false; }
void svc_storage_update(void) {}
void svc_service_leave(void) { g_leave_calls++; }
void svc_log(Api2LogSeverity s, const char *m) { (void)s; (void)m; g_logs++; }
void svc_logf(Api2LogSeverity s, const char *f, ...) { (void)s; (void)f; g_logs++; }

#include "../Services/svc_battery.c"

/* ---------------- helpers ---------------- */
static int charge_enabled(void)   /* CHARGE_EN is active LOW */
{
    return g_out[port_idx(CHARGE_EN_PORT)][PIN_INDEX(CHARGE_EN_PIN)] == 0;
}
static void set_vbus(int on)      { g_in[port_idx(VBUS_SENSE_PORT)][PIN_INDEX(VBUS_SENSE_PIN)] = on; }
static void set_chrg(int on)      { g_in[port_idx(CHARGE_SENSE_PORT)][PIN_INDEX(CHARGE_SENSE_PIN)] = !on; }   /* TP4056 CHRG: active LOW */
static void set_complete(int on)  { g_in[port_idx(STANDBY_SENSE_PORT)][PIN_INDEX(STANDBY_SENSE_PIN)] = !on; } /* STANDBY: active LOW */
static void set_vbat(unsigned mv) { g_adc.vbat_raw = (uint16_t)mv; g_adc.vrefint_raw = 1500; g_adc.valid = true; }

static void fresh(void)
{
    memset(&g_device_settings, 0, sizeof g_device_settings);
    memset(&g_system_state, 0, sizeof g_system_state);
    memset(g_in, 1, sizeof g_in);             /* idle: TP4056 outputs high = inactive */
    memset(g_out, 0, sizeof g_out);
    set_vbus(0);
    g_device_settings.vbat_scale_num = 1;
    g_device_settings.vbat_scale_den = 1;
    g_device_settings.vbat_offset_mv = 0;
    g_device_settings.battery_critical_mv = 3400;
    g_device_settings.battery_low_mv = 3600;
    g_device_settings.battery_charge_start_mv = 3700;
    memset(&g_adc, 0, sizeof g_adc);
    g_ms = 100000u;
    g_standby_calls = g_leave_calls = g_logs = 0;
    svc_battery_init();
    s_charge_inhibited = false;
}

static void tick(int n)
{
    for (int i = 0; i < n; ++i) { g_ms += 1000u; svc_battery_update(); }
}

static void settle(unsigned mv) { set_vbat(mv); tick(12); }      /* past the startup grace */

/* ---------------- conversions ---------------- */

TEST(the_battery_voltage_applies_scale_and_offset_and_never_goes_negative)
{
    fresh();
    g_device_settings.vbat_scale_num = 133;
    g_device_settings.vbat_scale_den = 100;
    CHECK_EQ(adc_to_vbat_mv(3000, 1500), 3990);               /* 3000 * 1.33 */
    g_device_settings.vbat_offset_mv = -90;
    CHECK_EQ(adc_to_vbat_mv(3000, 1500), 3900);
    g_device_settings.vbat_offset_mv = -5000;
    CHECK_EQ(adc_to_vbat_mv(3000, 1500), 0);                  /* clamped, not wrapped */
}

TEST(state_of_charge_follows_the_table_and_clamps)
{
    CHECK_EQ(vbat_to_soc(0), 0);
    CHECK_EQ(vbat_to_soc(3000), 0);
    CHECK_EQ(vbat_to_soc(3150), 5);                           /* halfway from 3000 (0 %) to 3300 (10 %) */
    CHECK_EQ(vbat_to_soc(3300), 10);
    CHECK_EQ(vbat_to_soc(3700), 50);
    CHECK_EQ(vbat_to_soc(4200), 100);
    CHECK_EQ(vbat_to_soc(4300), 100);
    unsigned prev = 0;
    for (unsigned mv = 2900; mv <= 4300; mv += 7) {           /* monotonic everywhere */
        unsigned s = vbat_to_soc((uint16_t)mv);
        CHECK(s >= prev);
        CHECK(s <= 100);
        prev = s;
    }
}

/* ---------------- classification ---------------- */

TEST(nothing_is_judged_before_the_startup_grace_has_passed)
{
    fresh();
    set_vbat(0);                                              /* an unfilled ADC reads zero */
    for (int i = 0; i < 9; ++i) {
        tick(1);
        CHECK_EQ(svc_battery_get_state(), BATTERY_NORMAL);
    }
    CHECK_EQ(g_standby_calls, 0);
}

TEST(the_thresholds_classify_normal_low_and_critical)
{
    fresh();
    settle(3800);  CHECK_EQ(svc_battery_get_state(), BATTERY_NORMAL);
    set_vbat(3600); tick(1); CHECK_EQ(svc_battery_get_state(), BATTERY_NORMAL);        /* at low: not yet low */
    set_vbat(3599); tick(1); CHECK_EQ(svc_battery_get_state(), BATTERY_LOW);
    set_vbat(3400); tick(1); CHECK_EQ(svc_battery_get_state(), BATTERY_LOW);
    set_vbat(3399); tick(1); CHECK_EQ(svc_battery_get_state(), BATTERY_CRITICAL);
    CHECK(g_system_state.battery_low && g_system_state.battery_critical);
    set_vbat(3800); tick(1);
    CHECK_EQ(svc_battery_get_state(), BATTERY_NORMAL);
    CHECK(!g_system_state.battery_low && !g_system_state.battery_critical);
}

TEST(a_zero_reading_after_startup_is_flagged_critical_but_never_powers_off)
{
    fresh();
    settle(3800);
    set_vbat(0);
    tick(30);
    CHECK_EQ(svc_battery_get_state(), BATTERY_CRITICAL);      /* an honest display */
    CHECK_EQ(g_standby_calls, 0);                             /* but an ADC fault must not shut the instrument down */
}

TEST(with_usb_power_the_state_reports_charging_full_or_normal_never_low)
{
    fresh();
    settle(3300);                                             /* a flat pack ... */
    set_vbus(1);
    tick(3);
    CHECK_EQ(svc_battery_get_state(), BATTERY_CHARGING);      /* charge policy enabled it */
    set_complete(1);
    tick(1);
    CHECK_EQ(svc_battery_get_state(), BATTERY_FULL);
    set_complete(0);
    settle(3900);
    tick(1);
    CHECK(svc_battery_get_state() == BATTERY_NORMAL || svc_battery_get_state() == BATTERY_CHARGING);
}

/* ---------------- charging ---------------- */

TEST(charging_starts_below_the_start_threshold_only_with_usb_after_two_samples)
{
    fresh();
    set_vbat(3650);
    tick(1);
    CHECK(!charge_enabled());
    set_vbus(1);
    tick(1);                                                  /* second valid sample */
    CHECK(charge_enabled());
}

TEST(a_full_enough_pack_is_left_alone)
{
    fresh();
    set_vbus(1);
    set_vbat(3900);
    tick(15);
    CHECK(!charge_enabled());
}

TEST(once_started_charging_stays_on_as_the_voltage_rises_until_complete)
{
    fresh();
    set_vbus(1);
    set_vbat(3600);
    tick(4);
    CHECK(charge_enabled());
    set_vbat(4150);
    tick(5);
    CHECK(charge_enabled());                                  /* latched: no oscillation around the threshold */
    set_complete(1);
    tick(1);
    CHECK(!charge_enabled());
    set_complete(0);
    tick(3);
    CHECK(!charge_enabled());                                 /* 4150 mV is above the start threshold: no restart */
}

TEST(force_charge_bypasses_the_threshold_but_not_the_usb_guard)
{
    fresh();
    settle(4000);
    svc_battery_force_charge();
    tick(2);
    CHECK(!charge_enabled());                                 /* no USB: ignored (and cleared) */
    CHECK(!svc_battery_is_force_charging());
    set_vbus(1);
    svc_battery_force_charge();
    tick(2);
    CHECK(charge_enabled());
    CHECK(svc_battery_is_force_charging());
    svc_battery_cancel_force_charge();
    tick(2);
    CHECK(charge_enabled());                                  /* the latch holds until complete or USB gone */
    set_complete(1);
    tick(1);
    CHECK(!charge_enabled());
}

TEST(inhibit_beats_everything_and_stays_until_cleared)
{
    fresh();
    set_vbus(1);
    set_vbat(3300);
    tick(4);
    CHECK(charge_enabled());
    svc_battery_set_charge_inhibit(true);
    tick(1);
    CHECK(!charge_enabled());                                 /* switches off an automatic charge */
    svc_battery_force_charge();
    tick(2);
    CHECK(!charge_enabled());                                 /* and wins over a forced one, clearing it */
    CHECK(!svc_battery_is_force_charging());
    set_vbus(0); tick(12); set_vbus(1); tick(3);              /* replug USB */
    CHECK(!charge_enabled());                                 /* still inhibited */
    svc_battery_set_charge_inhibit(false);
    tick(2);
    CHECK(charge_enabled());
}

/* ---------------- VBUS debounce ---------------- */

TEST(usb_arrival_registers_at_once_but_removal_needs_ten_seconds)
{
    fresh();
    settle(3800);
    set_vbus(1);
    tick(1);
    CHECK(g_system_state.usb_connected);
    set_vbus(0);
    tick(1);                                                  /* first absent tick starts the clock */
    for (int i = 0; i < 9; ++i) { tick(1); CHECK(g_system_state.usb_connected); }
    tick(1);
    CHECK(!g_system_state.usb_connected);
    set_vbus(1); tick(1);
    set_vbus(0); tick(5); set_vbus(1); tick(1); set_vbus(0); tick(5);   /* a blink restarts the grace */
    CHECK(g_system_state.usb_connected);
}

TEST(the_charger_outputs_are_only_trusted_while_vbus_is_really_present)
{
    fresh();
    settle(3800);
    set_vbus(1); set_chrg(1);
    tick(1);
    CHECK(svc_battery_is_charging());
    set_vbus(0);                                              /* raw VBUS gone: the TP4056 lines are garbage now */
    tick(1);
    CHECK(!svc_battery_is_charging());
}

/* ---------------- critical-battery shutdown ---------------- */

TEST(a_real_critical_battery_shuts_down_after_three_samples_and_two_seconds)
{
    fresh();
    set_vbat(3300);
    tick(11);                                                 /* updates 1..11: the 10th is the first CRITICAL (streak 1), the 11th streak 2 */
    CHECK_EQ(svc_battery_get_state(), BATTERY_CRITICAL);
    CHECK_EQ(g_standby_calls, 0);
    tick(1);                                                  /* update 12: streak 3, the 2 s timer arms */
    CHECK_EQ(g_standby_calls, 0);
    tick(1);                                                  /* update 13: 1 s into the countdown */
    CHECK_EQ(g_standby_calls, 0);
    tick(1);                                                  /* update 14: 2 s, low power entered */
    CHECK_EQ(g_standby_calls, 1);
    CHECK_EQ(g_leave_calls, 1);                               /* service mode ended on the way to sleep */
    CHECK_EQ(g_out[port_idx(PWR_5V_EN_PORT)][PIN_INDEX(PWR_5V_EN_PIN)], 0);       /* 5 V off (active HIGH) */
    CHECK_EQ(g_out[port_idx(PWR_3V3_EN_PORT)][PIN_INDEX(PWR_3V3_EN_PIN)], 1);     /* 3V3 off (active LOW) */
}

TEST(usb_appearing_during_the_countdown_cancels_the_shutdown)
{
    fresh();
    set_vbat(3300);
    tick(13);                                                 /* armed at update 12, 1 s in */
    CHECK_EQ(g_standby_calls, 0);
    set_vbus(1);
    tick(5);
    CHECK_EQ(g_standby_calls, 0);
    CHECK(svc_battery_get_state() != BATTERY_CRITICAL);       /* on external power the pack is being recovered */
}

TEST(a_low_but_not_critical_battery_never_shuts_down)
{
    fresh();
    set_vbat(3500);
    tick(120);
    CHECK_EQ(svc_battery_get_state(), BATTERY_LOW);
    CHECK_EQ(g_standby_calls, 0);
}

TEST(an_implausibly_low_voltage_is_treated_as_an_adc_fault_not_a_dead_battery)
{
    fresh();
    set_vbat(2400);                                           /* below the 2.5 V plausibility floor */
    tick(60);
    CHECK_EQ(svc_battery_get_state(), BATTERY_CRITICAL);
    CHECK_EQ(g_standby_calls, 0);
}

int main(void)
{
    RUN(the_battery_voltage_applies_scale_and_offset_and_never_goes_negative);
    RUN(state_of_charge_follows_the_table_and_clamps);
    RUN(nothing_is_judged_before_the_startup_grace_has_passed);
    RUN(the_thresholds_classify_normal_low_and_critical);
    RUN(a_zero_reading_after_startup_is_flagged_critical_but_never_powers_off);
    RUN(with_usb_power_the_state_reports_charging_full_or_normal_never_low);
    RUN(charging_starts_below_the_start_threshold_only_with_usb_after_two_samples);
    RUN(a_full_enough_pack_is_left_alone);
    RUN(once_started_charging_stays_on_as_the_voltage_rises_until_complete);
    RUN(force_charge_bypasses_the_threshold_but_not_the_usb_guard);
    RUN(inhibit_beats_everything_and_stays_until_cleared);
    RUN(usb_arrival_registers_at_once_but_removal_needs_ten_seconds);
    RUN(the_charger_outputs_are_only_trusted_while_vbus_is_really_present);
    RUN(a_real_critical_battery_shuts_down_after_three_samples_and_two_seconds);
    RUN(usb_appearing_during_the_countdown_cancels_the_shutdown);
    RUN(a_low_but_not_critical_battery_never_shuts_down);
    RUN(an_implausibly_low_voltage_is_treated_as_an_adc_fault_not_a_dead_battery);
    return test_summary();
}
