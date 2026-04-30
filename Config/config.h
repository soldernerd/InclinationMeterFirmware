#ifndef CONFIG_H
#define CONFIG_H

/* --- System tick --- */
#define SYSTICK_RATE_HZ                 1000
#define SYSTICK_PERIOD_MS               (1000 / SYSTICK_RATE_HZ)

/* --- Scheduler task periods (ms) --- */
#define DEFAULT_TASK_SENSORS_MS         100
#define DEFAULT_TASK_PROCESSING_MS      100
#define DEFAULT_TASK_DISPLAY_MS         100
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

/* --- Battery --- */
#define DEFAULT_BATTERY_CUTOFF_MV       3600
#define DEFAULT_BATTERY_LOW_PCT         20
#define DEFAULT_BATTERY_CRITICAL_PCT    5

/* --- EEPROM --- */
#define EEPROM_MAGIC                    0xA55A
#define EEPROM_SETTINGS_VERSION         0x0001
#define EEPROM_CALIBRATION_VERSION      0x0001
#define EEPROM_SETTINGS_ADDR            0x0000
#define EEPROM_CALIBRATION_ADDR         0x0100

#endif /* CONFIG_H */
