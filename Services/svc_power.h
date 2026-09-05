#ifndef SVC_POWER_H
#define SVC_POWER_H

/* Auto power-off (WP6). Watches for user inactivity and, after
 * g_device_settings.auto_poweroff_s seconds with no encoder rotation or
 * button activity, powers the instrument down into STM32 Standby via
 * svc_battery_enter_low_power() (the same path the critical-battery
 * shutdown uses). A wake source (encoder press) restarts the firmware
 * from reset, which starts a fresh idle window.
 *
 * Disabled when auto_poweroff_s == 0, and suppressed while USB or BLE is
 * connected, while charging, or while a measurement is running — sleeping
 * then would be either pointless (a level-triggered VBUS wake would fire
 * immediately) or destructive. */

void svc_power_init(void);
void svc_power_task(void);   /* every scheduler tick, after task_input */

/* Immediate, unconditional power-off — the SETTINGS "Power off" action
 * row calls this. Does not return. */
void svc_power_shutdown_now(void);

#endif /* SVC_POWER_H */
