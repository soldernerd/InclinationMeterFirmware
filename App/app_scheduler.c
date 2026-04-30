#include "app_scheduler.h"
#include "app_display.h"
#include "hal_systick.h"
#include "config.h"
#include "system_state.h"
#include <stddef.h>

typedef void (*TaskFn)(void);

typedef struct {
    TaskFn   task;
    uint32_t period_ms;
    uint32_t last_run_ms;
} SchedulerEntry;

static SchedulerEntry s_tasks[] = {
    { app_display_update, DEFAULT_TASK_DISPLAY_MS, 0 },
};
#define TASK_COUNT  (sizeof(s_tasks) / sizeof(s_tasks[0]))

void app_scheduler_init(void)
{
    g_device_settings.task_display_ms     = DEFAULT_TASK_DISPLAY_MS;
    g_device_settings.task_sensors_ms     = DEFAULT_TASK_SENSORS_MS;
    g_device_settings.task_processing_ms  = DEFAULT_TASK_PROCESSING_MS;
    g_device_settings.task_ble_ms         = DEFAULT_TASK_BLE_MS;
    g_device_settings.task_usb_ms         = DEFAULT_TASK_USB_MS;
    g_device_settings.task_battery_ms     = DEFAULT_TASK_BATTERY_MS;
    g_device_settings.task_temperature_ms = DEFAULT_TASK_TEMPERATURE_MS;
    g_device_settings.stream_interval_ms  = DEFAULT_STREAM_INTERVAL_MS;

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
