#ifndef CONFIG_H
#define CONFIG_H

/* --- System tick --- */
#define SYSTICK_RATE_HZ                 1000
#define SYSTICK_PERIOD_MS               (1000 / SYSTICK_RATE_HZ)

/* --- Scheduler task periods (ms) --- */
#define DEFAULT_TASK_SENSORS_MS         100
#define DEFAULT_TASK_PROCESSING_MS      100
#define DEFAULT_TASK_DISPLAY_MS         100
#define DEFAULT_TASK_LED_MS             250     /* status LED toggle period -> 2 Hz blink */
#define DEFAULT_TASK_BLE_MS             100
#define DEFAULT_TASK_USB_MS             100
#define DEFAULT_TASK_BATTERY_MS         1000
#define DEFAULT_TASK_TEMPERATURE_MS     10000

/* --- Data streaming --- */
#define DEFAULT_STREAM_INTERVAL_MS      200

/* --- Settling --- */
#define SETTLING_BUFFER_SIZE            256
#define DEFAULT_SETTLING_THRESHOLD      10
#define DEFAULT_SETTLING_TIMEOUT_MS     30000

/* --- Complementary filter --- */
#define DEFAULT_FILTER_CUTOFF_HZ_NUM    1
#define DEFAULT_FILTER_CUTOFF_HZ_DEN    2

/* --- Battery ---
 * Voltage-based thresholds (2026-08-17, user-specified): >=3.80V normal,
 * 3.65V-3.80V low, <3.65V critical. Replaced the previous SOC%-based
 * low/critical thresholds — those and the raw voltage check were two
 * different views of the same underlying measurement (battery voltage),
 * which was redundant; voltage is the more direct, sensor-agnostic
 * quantity so it's now the sole classification input (see
 * Services/svc_battery.c). SOC% is still computed/exposed for display,
 * just no longer used for state classification. */
#define DEFAULT_BATTERY_CRITICAL_MV     3650
#define DEFAULT_BATTERY_LOW_MV          3800

/* --- EEPROM --- */
#define EEPROM_MAGIC                    0xA55A
#define EEPROM_SETTINGS_VERSION         0x0002  /* bumped: battery_cutoff_mv/
                                                   * battery_low_pct/
                                                   * battery_critical_pct
                                                   * replaced by
                                                   * battery_critical_mv/
                                                   * battery_low_mv */
#define EEPROM_CALIBRATION_VERSION      0x0001
#define EEPROM_SETTINGS_ADDR            0x0000
#define EEPROM_CALIBRATION_ADDR         0x0100

#endif /* CONFIG_H */
