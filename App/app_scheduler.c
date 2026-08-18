#include "app_scheduler.h"
#include "app_display.h"
#include "app_leds.h"
#include "app_ui.h"
#include "drv_tmp236.h"
#include "drv_buzzer.h"
#include "hal_adc.h"
#include "hal_systick.h"
#include "svc_api.h"
#include "svc_battery.h"
#include "svc_ble.h"
#include "svc_uart.h"
#include "svc_measurement.h"
#include "svc_signal_analysis.h"
#include "svc_storage.h"
#include "svc_input.h"
#include "svc_usb.h"
#include "config.h"
#include "system_state.h"
#include <stddef.h>

typedef void (*TaskFn)(void);

typedef struct {
    TaskFn   task;
    uint32_t period_ms;
    uint32_t last_run_ms;
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
    /* drv_tmp236_start_read just calls hal_adc_start; safe to call even if
     * a scan is already running — hal_adc_start no-ops in that case. */
    (void)drv_tmp236_start_read();
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
    app_display_update();
}

static void task_leds(void)
{
    app_leds_task();
}

static void task_usb(void)         { svc_usb_update();         }
static void task_ble(void)         { svc_ble_update();         }
static void task_uart(void)        { svc_uart_update();        }
static void task_api(void)         { svc_api_update();         }
static void task_measurement(void) { svc_measurement_update(); }
static void task_signal_analysis(void) { svc_signal_analysis_update(); }

/* ---- task table ----
 * Order matters when multiple tasks share a tick: task_ui before
 * task_display so display sees a fresh redraw_needed the same tick. */

static SchedulerEntry s_tasks[] = {
    { task_adc,         0,                   0 },
    { task_temperature, 0,                   0 },
    { task_battery,     0,                   0 },
    { task_storage,     0,                   0 },
    { task_input,       0,                   0 },
    { task_buzzer,      0,                   0 },
    { task_usb,         0,                    0 },
    { task_ble,         0,                    0 },
    { task_uart,        DEFAULT_TASK_UART_MS, 0 },
    { task_api,         0,                    0 },
    { task_measurement, 0,                    0 },
    { task_signal_analysis, 0,                0 },
    { task_ui,          0,                    0 },
    { task_display,     0,                    0 },
    { task_leds,        DEFAULT_TASK_LED_MS, 0 },
};
#define TASK_COUNT  (sizeof(s_tasks) / sizeof(s_tasks[0]))

static bool s_booted = false;

void app_scheduler_reload_periods(void)
{
    /* Periods are loaded from g_device_settings, which svc_storage_init
     * has already populated (or seeded with defaults from config.h).
     * Deliberately does NOT touch last_run_ms — safe to call re-entrantly
     * (e.g. from App/app_ui.c's commit_edit(), itself running from inside
     * task_ui, mid-iteration of app_scheduler_run()'s own for-loop). This
     * is the preferred entry point for a runtime settings change; only
     * app_scheduler_init() also resets last_run_ms, and only once. */
    s_tasks[0].period_ms  = g_device_settings.task_sensors_ms;
    s_tasks[1].period_ms  = g_device_settings.task_temperature_ms;
    s_tasks[2].period_ms  = g_device_settings.task_battery_ms;
    s_tasks[3].period_ms  = SYSTICK_PERIOD_MS;     /* storage — every tick */
    s_tasks[4].period_ms  = SYSTICK_PERIOD_MS;     /* input — every tick */
    s_tasks[5].period_ms  = SYSTICK_PERIOD_MS;     /* buzzer — every tick */
    s_tasks[6].period_ms  = g_device_settings.task_usb_ms;
    s_tasks[7].period_ms  = g_device_settings.task_ble_ms;        /* ble — fast poll for UART command/response latency */
    s_tasks[9].period_ms  = g_device_settings.task_sensors_ms;   /* api — poll rate for stream/progress checks */
    s_tasks[10].period_ms = g_device_settings.task_sensors_ms;   /* measurement */
    s_tasks[11].period_ms = g_device_settings.task_sensors_ms;   /* signal analysis --
                                                                    * finalizes at most this
                                                                    * often; see
                                                                    * svc_signal_analysis.c */
    s_tasks[12].period_ms = g_device_settings.task_display_ms;   /* ui */
    s_tasks[13].period_ms = g_device_settings.task_display_ms;   /* display */
    /* s_tasks[8] (UART) and s_tasks[14] (LEDs) periods are the fixed
     * literals set in the table above — not user/BLE-configurable like
     * the others (DeviceSettings has no room left for another field —
     * see config.h's DEFAULT_TASK_UART_MS comment). */
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
    }
}
