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
 * Voltage-based thresholds (2026-09-01, user-specified):
 *   >= 3.70V  normal
 *   < 3.70V   start charging (if USB present) — battery_charge_start_mv
 *   < 3.60V   low-battery warning on the display — battery_low_mv
 *   < 3.40V   critical: 2s warning then Standby power-off — battery_critical_mv
 * Voltage is the sole classification input (see Services/svc_battery.c);
 * SOC% is still computed/exposed for display, not used for classification. */
#define DEFAULT_BATTERY_CRITICAL_MV     3400
#define DEFAULT_BATTERY_LOW_MV          3600
#define DEFAULT_BATTERY_CHARGE_START_MV 3700

/* --- Auto power-off (WP6) ---
 * Idle seconds (no encoder activity) before the instrument powers itself
 * down into Standby. 0 disables it. EEPROM-backed (battery page), get/set
 * over the API (Settings resource 0x1B) and on the SETTINGS screen. */
#define DEFAULT_AUTO_POWEROFF_S        300     /* 5 minutes */

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

/* --- EEPROM (REV B, 2026-08-17: per-subsystem page split) ---
 * DeviceSettings used to live under ONE version number covering the
 * whole struct — any single field addition (most recently
 * encoder_counts_per_detent, version 0x0004) forced every OTHER
 * subsystem's saved data to be discarded and reseeded to defaults too,
 * confirmed as real project debt during code review (see
 * docs/wp2-5_rebase_status.md). Fixed by giving each subsystem its own
 * page — its own magic/version/CRC header — so a layout change in one
 * no longer touches the others. This changes the address map, not just
 * field content, so any prior EEPROM contents under the old single-page
 * scheme are void either way; harmless, since no hardware has been
 * calibrated/flashed yet. Services/svc_storage.c is the only consumer
 * of the _ADDR/_VERSION pairs below — see its SettingsSection table.
 *
 * 256 bytes/page (24LC256 has 32 KB total, so this uses well under 5%
 * of it) leaves each subsystem generous room to grow without ever
 * needing to shift addresses again. */
#define EEPROM_MAGIC                     0xA55A

#define EEPROM_SCHEDULER_SETTINGS_ADDR    0x0000  /* task periods, stream interval, settling, filter */
#define EEPROM_SCHEDULER_SETTINGS_VERSION 0x0001
#define EEPROM_BATTERY_SETTINGS_ADDR      0x0100  /* thresholds + ADC divider scale */
#define EEPROM_BATTERY_SETTINGS_VERSION   0x0003  /* 0x0002: added battery_charge_start_mv
                                                     and retuned thresholds (WP2 debug).
                                                     0x0003 (WP6): the page's alignment pad
                                                     became auto_poweroff_s — a stored v2
                                                     page is discarded and reseeded from
                                                     DEFAULT_* (which already hold the tuned
                                                     threshold values). */
#define EEPROM_TMP236_SETTINGS_ADDR       0x0200  /* on-board temp sensor piecewise-linear constants */
#define EEPROM_TMP236_SETTINGS_VERSION    0x0001
#define EEPROM_LM35_SETTINGS_ADDR         0x0300  /* external temp sensor scale */
#define EEPROM_LM35_SETTINGS_VERSION      0x0001
#define EEPROM_ENCODER_SETTINGS_ADDR      0x0400  /* quadrature counts/detent */
#define EEPROM_ENCODER_SETTINGS_VERSION   0x0001

#define EEPROM_CALIBRATION_ADDR         0x0500     /* moved from 0x0100 */
#define EEPROM_CALIBRATION_VERSION      0x0001

/* --- USB HID (WP4) ---
 * VID 0x04D8 = Microchip Technology. Other soldernerd projects (notably
 * SolarChargerRevE) use this VID with project-specific PIDs granted by
 * Microchip. We adopt the same VID for consistency in the soldernerd
 * USB device family.
 *
 * NOTE: Microchip's grant strictly covers Microchip-MCU-based products.
 * This firmware runs on an STM32, so the VID match is informal. Replace
 * with a pid.codes / Open Stella allocation if a clean licensing story
 * is needed for distribution.
 *
 * SolarCharger PID is 0xF08E. Picking 0xF08F here so the two don't
 * collide on the same host. */
#define USB_VID                         0x04D8
#define USB_PID                         0xF08F
#define USB_HID_REPORT_SIZE             64
#define USB_MANUFACTURER_STR            "soldernerd"
#define USB_PRODUCT_STR                 "InclinationMeter"
#define USB_SERIAL_STR                  "001"

/* RN4871 advertised name. Set via the module's "S-,<name>" command, which
 * serializes it as "<name>-<last 2 MAC bytes>" (e.g. "Leveltronic-A1B2").
 * Keep <= 15 chars so the serialized form fits the BLE advertisement. */
#define BLE_DEVICE_NAME                 "Leveltronic"

/* API v2 (WP11). */
#define API_RX_PACKET_TIMEOUT_MS        250U   /* abandon a stalled partial packet */
#define SVC_LOG_MSG_MAX                 48U    /* longest debug-log line kept, bytes */

/* Per-transport outbound TX frame ring (CLAUDE.md §8.3, Services/svc_txframe.c).
 * One per transport (USB, BLE, UART). Power of two. 1 KiB holds ~8 full
 * 128-byte frames or many small subscription pushes while the link
 * drains, with 64 bytes reserved so a command response can always get
 * out even when a stream has filled the rest. */
#define API_TX_RING_SIZE               1024U

/* --- AD9833 waveform generator DAC (WP7) ---
 * MCLK is fed from TIM1_CH4 / PC11, CubeMX-configured (Core/Src/tim.c) for
 * a fixed 64 MHz / (2 x 6) = 5.3333... MHz square wave — see
 * hal_tim_dac_clock_start(). AD9833 output frequency is
 * FREQREG x MCLK / 2^28 (datasheet "Frequency and Phase Registers"). To
 * land exactly 2048 MCLK cycles per output wave (fOUT = MCLK/2048 ~=
 * 2604.2 Hz), FREQREG = 2^28 / 2048 = 2^17 = 131072 exactly — no rounding
 * error, which is why 2048 cycles/wave was the target. */
#define AD9833_FREQREG                 131072UL   /* = 0x00020000, = 2^17 */

#endif /* CONFIG_H */
