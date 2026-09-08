#include "app_scheduler.h"
#include "app_display.h"
#include "app_leds.h"
#include "app_ui.h"
#include "drv_tmp236.h"
#include "drv_lm35.h"
#include "drv_bme280.h"
#include "drv_sharp_lcd.h"
#include "drv_buzzer.h"
#include "hal_adc.h"
#include "hal_systick.h"
#include "hal_power.h"
#include "svc_api.h"
#include "svc_battery.h"
#include "svc_signal_analysis.h"
#include "svc_storage.h"
#include "svc_input.h"
#include "svc_usb.h"
#include "svc_ble.h"
#include "svc_uart.h"
#include "svc_power.h"
#include "svc_log.h"
#include "svc_powertest.h"
#include "config.h"
#include "system_state.h"
#include <stddef.h>

typedef void (*TaskFn)(void);

typedef struct {
    TaskFn          task;
    const uint16_t *period_src;   /* NULL: period_ms is a fixed literal.
                                    non-NULL: app_scheduler_reload_periods()
                                    copies *period_src (an EEPROM-backed
                                    g_device_settings field) into period_ms,
                                    at boot and on every runtime settings
                                    change. */
    uint32_t        period_ms;
    uint32_t        last_run_ms;
} SchedulerEntry;

/* ---- task wrappers ---- */

static void task_adc(void)
{
    /* Restart the scan as soon as the previous one is consumed. The
     * three downstream readers (drv_tmp236, svc_battery, optional VREFINT
     * cross-check) run on their own slower periods and just sample the
     * latest hal_adc results. */
    if (hal_adc_is_ready()) {
        hal_adc_start();
    }
}

static void task_temperature(void)
{
    tmp236_data_t data;
    if (drv_tmp236_get_result(&data) == DRV_OK) {
        g_system_state.temperature_cdeg = data.temp_cdeg;
    }

    /* LM35 external temp (WP11) — same ADC scan, different channel. ok
     * stays false while the reading is out of the LM35's datasheet range
     * (disconnected / shorted). */
    lm35_data_t lm;
    if (drv_lm35_get_result(&lm) == DRV_OK) {
        g_system_state.temp_ext_cdeg = lm.temp_cdeg;
        g_system_state.temp_ext_ok   = true;
    } else {
        g_system_state.temp_ext_ok   = false;
    }

    /* drv_tmp236_start_read just calls hal_adc_start; safe to call even if
     * a scan is already running — hal_adc_start no-ops in that case. */
    (void)drv_tmp236_start_read();
}

static void task_bme280(void)
{
    /* One synchronous trigger+poll+read+compensate cycle per call
     * (bounded, see config.h/drv_bme280.h) -- mirrors
     * task_temperature()'s TMP236 pattern above, just with the whole
     * cycle inside drv_bme280_update() instead of split across a
     * start/get pair, since there's no async ADC hardware to wait on
     * here. bme280_ok is keyed off update()'s own return, not
     * get_result()'s -- get_result() keeps returning DRV_OK (the last
     * good reading) even after the sensor stops responding, which
     * would otherwise leave bme280_ok stuck true forever. */
    bool ok = (drv_bme280_update() == DRV_OK);
    static bool s_bme_was_ok = false;
    if (ok != s_bme_was_ok) {
        svc_log(API2_LOG_INFO, ok ? "bme280: connected" : "bme280: lost");
        s_bme_was_ok = ok;
    }
    g_system_state.bme280_ok = ok;
    if (ok) {
        bme280_data_t data;
        if (drv_bme280_get_result(&data) == DRV_OK) {
            g_system_state.bme280_temp_cdeg         = data.temp_cdeg;
            g_system_state.bme280_pressure_pa       = data.pressure_pa;
            g_system_state.bme280_humidity_centipct = data.humidity_centipct;
        }
    }
}

static void task_battery(void)
{
    svc_battery_update();
}

static void task_storage(void)
{
    svc_storage_update();
}

static void task_input(void)
{
    /* ENC_1SW/ENC_2SW aren't EXTI-capable on this pinout (see
     * pin_config.h) so they need polling; also mirrors the EXTI-driven
     * encoder rotation counts into g_system_state. Runs every tick so
     * task_ui (slower, task_display_ms) doesn't miss a button press
     * latched between its ticks. */
    svc_input_update();
}

static void task_buzzer(void)
{
    drv_buzzer_update();
}

static void task_ui(void)
{
    app_ui_update();
}

static void task_display(void)
{
    /* Finish releasing CS on any flush whose DMA phase already completed,
     * before deciding whether to kick off a new one this tick. */
    drv_sharp_lcd_update();
    /* App layer owns the system-state health flag; the driver only tracks
     * its own s_ok (see drv_sharp_lcd_ok()). */
    g_system_state.display_ok = drv_sharp_lcd_ok();
    app_display_update();
}

static void task_leds(void)
{
    app_leds_task();
}

static void task_usb(void)         { svc_usb_update();         }
static void task_ble(void)         { svc_ble_task();           }
static void task_uart(void)        { svc_uart_update();        }
static void task_api(void)
{
    /* Both every tick: svc_api_update() drains the debug-log push queue
     * (self-limited to a few lines per call); the subscriptions poll must
     * be every tick so a 50 ms Measurements interval isn't coarsened to a
     * slower cadence — see svc_api.c's comment on that function. */
    svc_api_update();
    svc_api_measurement_subscriptions_update();
    svc_api_topic_subscriptions_update();
}
static void task_signal_analysis(void) { svc_signal_analysis_update();
                                         svc_signal_analysis_check_integrity(); }
static void task_power(void)           { svc_power_task();             }

/* ---- task table ----
 * Order matters when multiple tasks share a tick: task_ui before
 * task_display so display sees a fresh redraw_needed the same tick;
 * task_usb/task_ble/task_uart before task_api so a command received this
 * tick can get its response sent the same tick.
 *
 * period_src is the single source of truth for each task's period — a
 * pointer to its g_device_settings field, or NULL for a fixed literal.
 * app_scheduler_reload_periods() is one loop over this table, so
 * inserting / removing / reordering a row can no longer silently
 * mis-assign every period after it (which it used to, when reload keyed
 * off hard-coded indices). */
#define EVERY_TICK  SYSTICK_PERIOD_MS

static SchedulerEntry s_tasks[] = {
    { task_adc,             &g_device_settings.task_sensors_ms,     0,                      0 },
    { task_temperature,     &g_device_settings.task_temperature_ms, 0,                      0 },
    { task_battery,         &g_device_settings.task_battery_ms,     0,                      0 },
    { task_storage,         NULL,                                  EVERY_TICK,             0 },
    { task_input,           NULL,                                  EVERY_TICK,             0 },
    { task_power,           NULL,                                  EVERY_TICK,             0 },  /* idle-timer accuracy */
    { task_buzzer,          NULL,                                  EVERY_TICK,             0 },
    { task_usb,             &g_device_settings.task_usb_ms,         0,                      0 },
    { task_ble,             &g_device_settings.task_ble_ms,         0,                      0 },
    { task_uart,            NULL,                                  EVERY_TICK,             0 },  /* RX latency + TX drain; no EEPROM setting */
    { task_api,             NULL,                                  EVERY_TICK,             0 },  /* subscription timing accuracy */
    { task_signal_analysis, &g_device_settings.task_sensors_ms,     0,                      0 },  /* finalizes at most this often; batches complete faster (svc_signal_analysis.c) */
    { task_ui,              &g_device_settings.task_display_ms,     0,                      0 },
    { task_display,         NULL,                                  EVERY_TICK,             0 },  /* renders DISPLAY_PAGES_PER_TICK bands/call (app_display.c); pump every tick to finish a redraw promptly */
    { task_leds,            NULL,                                  DEFAULT_TASK_LED_MS,    0 },  /* not user/BLE-configurable */
    { task_bme280,          NULL,                                  DEFAULT_TASK_BME280_MS, 0 },  /* not user/BLE-configurable */
};
#define TASK_COUNT  (sizeof(s_tasks) / sizeof(s_tasks[0]))

static bool s_booted = false;

void app_scheduler_reload_periods(void)
{
    /* g_device_settings has already been populated by svc_storage_init
     * (or seeded with config.h defaults). Deliberately does NOT touch
     * last_run_ms — safe to call re-entrantly (e.g. from App/app_ui.c's
     * commit_edit(), itself running inside task_ui, mid-iteration of
     * app_scheduler_run()'s own for-loop). Preferred entry point for a
     * runtime settings change; only app_scheduler_init() also resets
     * last_run_ms, and only once. */
    for (size_t i = 0; i < TASK_COUNT; ++i) {
        if (s_tasks[i].period_src != NULL) {
            s_tasks[i].period_ms = *s_tasks[i].period_src;
        }
    }
}

void app_scheduler_init(void)
{
    app_scheduler_reload_periods();

    if (s_booted) {
        /* Re-entrant call — a previous code-review pass found a real bug
         * where a settings-save path called this (instead of
         * app_scheduler_reload_periods()) from inside a running task,
         * stamping every task's last_run_ms with a freshly-sampled tick
         * and causing an unsigned-wraparound spurious re-fire for any
         * task not yet reached that pass. Rather than rely solely on
         * callers picking the right function, degrade safely here too:
         * only the true first (boot) call resets last_run_ms. */
        return;
    }
    s_booted = true;

    uint32_t now = hal_systick_get_ms();
    for (size_t i = 0; i < TASK_COUNT; ++i) {
        s_tasks[i].last_run_ms = now;
    }
}

void app_scheduler_run(void)
{
    while (1) {
        /* Deliberately NOT hal_systick_elapsed_ms() here: `now` is one
         * consistent snapshot reused for every task's due-check and its
         * last_run_ms stamp this pass. hal_systick_elapsed_ms() samples
         * a fresh tick internally, which would let each task's check
         * (and its last_run_ms assignment) drift against a slightly
         * different `now`, and desync the two from each other — a
         * real, if minor, behavior change from re-sampling per task
         * instead of once per pass. Same wrap-safe unsigned-subtract
         * math either way, just against a locally cached `now`. */
        uint32_t now = hal_systick_get_ms();
        for (size_t i = 0; i < TASK_COUNT; ++i) {
            if ((uint32_t)(now - s_tasks[i].last_run_ms) >= s_tasks[i].period_ms) {
                s_tasks[i].task();
                s_tasks[i].last_run_ms = now;
            }
        }
        /* Normally the loop busy-spins (PWRTEST_CPU_SPIN set). The power
         * investigation can clear that bit to sleep the core between ISRs
         * instead — SysTick (1 ms) always wakes it, so at most 1 ms of
         * scheduling latency is added. */
        if (!svc_powertest_cpu_spin()) {
            hal_power_wait_for_interrupt();
        }
    }
}
