#ifndef APP_SCHEDULER_H
#define APP_SCHEDULER_H

void app_scheduler_init(void);
void app_scheduler_run(void);

/* Reloads task periods from g_device_settings without resetting
 * last_run_ms — safe to call re-entrantly (e.g. from inside a task),
 * unlike app_scheduler_init(). See app_scheduler.c for why. */
void app_scheduler_reload_periods(void);

#endif /* APP_SCHEDULER_H */
