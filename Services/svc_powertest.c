#include "svc_powertest.h"
#include "svc_signal_analysis.h"
#include "svc_log.h"
#include "hal_power.h"
#include "hal_tim.h"
#include "hal_gpio.h"
#include "drv_ad9833.h"
#include "drv_ads131m04.h"
#include "drv_rn4871.h"
#include "pin_config.h"

static uint32_t s_mask = PWRTEST_ALL;

/* --- per-subsystem effect (on == keep/restore, off == cut for measurement) --- */

static void set_5v_rail(bool on)
{
    hal_power_rail_5v_set(on);
}

static void set_3v3_rail(bool on)
{
    hal_power_rail_3v3_set(on);
}

static void set_ad9833(bool on)
{
    if (on) {
        (void)drv_ad9833_init();          /* restarts MCLK + full register load */
    } else {
        (void)drv_ad9833_sleep();         /* RESET + SLEEP1 + SLEEP12 */
        hal_tim_dac_clock_stop();         /* kill the external MCLK on PC11 */
    }
}

static void set_ads131m04(bool on)
{
    if (on) {
        hal_gpio_set(ADC_SYNC_RESET_PORT, ADC_SYNC_RESET_PIN, true);   /* release reset */
        hal_tim_adc_clock_start();
    } else {
        svc_signal_analysis_stop();        /* stop the acquisition trigger, if running */
        hal_gpio_set(ADC_SYNC_RESET_PORT, ADC_SYNC_RESET_PIN, false);  /* hold in reset */
        hal_tim_adc_clock_stop();          /* kill the external MCLK on PB10 */
    }
}

static void set_ble(bool on)
{
    /* BLE_RESET active-LOW (low = held in reset). Restoring only releases
     * reset — the RN4871 needs drv_rn4871 to re-run its config sequence
     * for a full recovery; a reboot is cleaner. */
    hal_gpio_set(BLE_RESET_PORT, BLE_RESET_PIN, on);
    if (on) {
        (void)drv_rn4871_init();
    }
}

static void set_display(bool on)
{
    /* Order matters: VCOM must never stop while the panel is powered
     * (permanent damage), and must be toggling before the panel powers up. */
    if (on) {
        hal_tim_vcom_start();
        hal_gpio_set(DISP_ON_PORT, DISP_ON_PIN, true);
    } else {
        hal_gpio_set(DISP_ON_PORT, DISP_ON_PIN, false);
        hal_tim_vcom_stop();
    }
}

static void set_leds(bool on)
{
    if (!on) {
        hal_gpio_set(LED_PWR_PORT, LED_PWR_PIN, false);
        hal_gpio_set(LED_STS_PORT, LED_STS_PIN, false);
    }
    /* on: app_leds_task() resumes driving them next tick (it polls
     * svc_powertest_leds_on()). */
}

void svc_powertest_init(void)
{
    s_mask = PWRTEST_ALL;   /* everything already on at boot — no effects to apply */
}

void svc_powertest_apply(uint32_t mask)
{
    mask &= PWRTEST_KNOWN_BITS;
    uint32_t changed = s_mask ^ mask;

    if (changed & PWRTEST_AD9833)    set_ad9833(mask & PWRTEST_AD9833);
    if (changed & PWRTEST_ADS131M04) set_ads131m04(mask & PWRTEST_ADS131M04);
    if (changed & PWRTEST_BLE)       set_ble(mask & PWRTEST_BLE);
    if (changed & PWRTEST_DISPLAY)   set_display(mask & PWRTEST_DISPLAY);
    if (changed & PWRTEST_LEDS)      set_leds(mask & PWRTEST_LEDS);
    /* Rails last: dropping 3V3/5V can strand a peripheral mid-transaction,
     * so cut the peripherals above it first (and bring rails up before
     * the peripherals when restoring — a second apply() call re-runs the
     * peripheral bits harmlessly). */
    if (changed & PWRTEST_5V_RAIL)   set_5v_rail(mask & PWRTEST_5V_RAIL);
    if (changed & PWRTEST_3V3_RAIL)  set_3v3_rail(mask & PWRTEST_3V3_RAIL);

    s_mask = mask;
    svc_logf(API2_LOG_WARN, "pwrtest: mask=0x%02lX", (unsigned long)s_mask);
}

uint32_t svc_powertest_mask(void) { return s_mask; }
bool svc_powertest_cpu_spin(void) { return (s_mask & PWRTEST_CPU_SPIN) != 0UL; }
bool svc_powertest_leds_on(void)  { return (s_mask & PWRTEST_LEDS)     != 0UL; }
