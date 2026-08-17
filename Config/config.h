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

/* --- ADC/sensor scaling calibration (2026-08-17) ---
 * General project rule: no numeric calibration constant lives only in
 * flash — everything here is a *default seed* for DeviceSettings, which
 * is EEPROM-backed (see system_state.h/svc_storage.c). The actual
 * consuming code (Services/svc_battery.c, Drivers_App/drv_tmp236.c) reads
 * g_device_settings.*, never these macros directly, past first boot. */
#define DEFAULT_VBAT_SCALE_NUM          133     /* 100k/33k divider — see pin_config.h */
#define DEFAULT_VBAT_SCALE_DEN          33
/* TMP236 piecewise-linear transfer function (TI datasheet SBOS857E,
 * Table 2) — see Drivers_App/drv_tmp236.c for the equation this feeds. */
#define DEFAULT_TMP236_SEG1_VOFFS_MV    400
#define DEFAULT_TMP236_SEG1_NUM         200
#define DEFAULT_TMP236_SEG1_DEN         39
#define DEFAULT_TMP236_SEG_BOUNDARY_MV  2350
#define DEFAULT_TMP236_SEG2_VOFFS_MV    2350
#define DEFAULT_TMP236_SEG2_NUM         1000
#define DEFAULT_TMP236_SEG2_DEN         197
#define DEFAULT_TMP236_SEG2_TINFL_CDEG  10000
/* LM35 (TI datasheet SNIS159H): 10 mV/°C, no offset. Not yet consumed by
 * any driver — TEMP_SENSE_EXT has no driver yet — kept here so the
 * default is defined in the same place as everything else once it is. */
#define DEFAULT_LM35_SCALE_MV_PER_C     10

/* --- Encoder (WP3) ---
 * Raw quadrature transitions per mechanical detent — NOT confirmed
 * against this board's actual encoder part (2026-08-17 user note: "not
 * even sure about the edges per dent"). Same category as the ADC/sensor
 * calibration constants above (unconfirmed-against-real-hardware value),
 * so it follows the same rule: EEPROM-backed DeviceSettings field, not a
 * bare #define — see App/app_ui.c's consume_detents(). */
#define DEFAULT_ENCODER_COUNTS_PER_DETENT  4

/* --- EEPROM --- */
#define EEPROM_MAGIC                    0xA55A
#define EEPROM_SETTINGS_VERSION         0x0004  /* bumped: added
                                                   * encoder_counts_per_detent —
                                                   * see DEFAULT_ENCODER_COUNTS_PER_DETENT
                                                   * above. Previous bump (0x0003)
                                                   * added vbat_scale_num/den,
                                                   * tmp236_seg1/seg2_*,
                                                   * lm35_scale_mv_per_c. */
#define EEPROM_CALIBRATION_VERSION      0x0001
#define EEPROM_SETTINGS_ADDR            0x0000
#define EEPROM_CALIBRATION_ADDR         0x0100

#endif /* CONFIG_H */
