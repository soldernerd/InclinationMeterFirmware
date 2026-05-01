#include "app_scheduler.h"
#include "app_display.h"
#include "app_leds.h"
#include "drv_tmp236.h"
#include "drv_buzzer.h"
#include "hal_adc.h"
#include "hal_systick.h"
#include "svc_battery.h"
#include "svc_storage.h"
#include "svc_input.h"
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
    /* Encoder turns are EXTI-driven (see Drivers_App/drv_encoder.c) and
     * need no polling themselves, but button state (ENC_1SW/ENC_2SW are
     * not EXTI-capable on this pinout — see pin_config.h) and the
     * "did anything change since last tick" edge/beep logic do. */
    svc_input_update();
}

static void task_buzzer(void)
{
    drv_buzzer_update();
}

static void task_display(void)
{
    app_display_update();
}

static void task_leds(void)
{
    app_leds_task();
}

/* ---- task table ---- */

static SchedulerEntry s_tasks[] = {
    { task_adc,         0,                   0 },
    { task_temperature, 0,                   0 },
    { task_battery,     0,                   0 },
    { task_storage,     0,                   0 },
    { task_input,       0,                   0 },
    { task_buzzer,      0,                   0 },
    { task_display,     0,                   0 },
    { task_leds,        DEFAULT_TASK_LED_MS, 0 },
};
#define TASK_COUNT  (sizeof(s_tasks) / sizeof(s_tasks[0]))

void app_scheduler_init(void)
{
    /* Periods are loaded from g_device_settings, which svc_storage_init
     * has already populated (or seeded with defaults from config.h). */
    s_tasks[0].period_ms = g_device_settings.task_sensors_ms;
    s_tasks[1].period_ms = g_device_settings.task_temperature_ms;
    s_tasks[2].period_ms = g_device_settings.task_battery_ms;
    s_tasks[3].period_ms = SYSTICK_PERIOD_MS;     /* every tick */
    s_tasks[4].period_ms = SYSTICK_PERIOD_MS;     /* input — every tick */
    s_tasks[5].period_ms = SYSTICK_PERIOD_MS;     /* buzzer — every tick */
    s_tasks[6].period_ms = g_device_settings.task_display_ms;
    /* s_tasks[7] (LEDs) period is the fixed literal set in the table above —
     * not user/BLE-configurable like the others. */

    uint32_t now = hal_systick_get_ms();
    for (size_t i = 0; i < TASK_COUNT; ++i) {
        s_tasks[i].last_run_ms = now;
    }
}

void app_scheduler_run(void)
{
    while (1) {
        uint32_t now = hal_systick_get_ms();
        for (size_t i = 0; i < TASK_COUNT; ++i) {
            if ((uint32_t)(now - s_tasks[i].last_run_ms) >= s_tasks[i].period_ms) {
                s_tasks[i].task();
                s_tasks[i].last_run_ms = now;
            }
        }
    }
}
