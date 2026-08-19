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
#define DEFAULT_TASK_UART_MS            100     /* fixed, not EEPROM-configurable like
                                                   * task_ble_ms/task_usb_ms — see
                                                   * App/app_scheduler.c; DeviceSettings has
                                                   * no headroom left for another field (it's
                                                   * sent whole in one GET/SET_SETTINGS packet,
                                                   * already at the 60-byte payload ceiling) */
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
/* LM35 (TI datasheet SNIS159H): 10 mV/°C, no offset. Consumed by
 * Drivers_App/drv_lm35.c (WP11). */
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
#define EEPROM_BATTERY_SETTINGS_VERSION   0x0001
#define EEPROM_TMP236_SETTINGS_ADDR       0x0200  /* on-board temp sensor piecewise-linear constants */
#define EEPROM_TMP236_SETTINGS_VERSION    0x0001
#define EEPROM_LM35_SETTINGS_ADDR         0x0300  /* external temp sensor scale */
#define EEPROM_LM35_SETTINGS_VERSION      0x0001
#define EEPROM_ENCODER_SETTINGS_ADDR      0x0400  /* quadrature counts/detent */
#define EEPROM_ENCODER_SETTINGS_VERSION   0x0001

#define EEPROM_CALIBRATION_ADDR         0x0500     /* moved from 0x0100 */
#define EEPROM_CALIBRATION_VERSION      0x0001

#define EEPROM_BLE_SETTINGS_ADDR         0x0600  /* ble_configured */
#define EEPROM_BLE_SETTINGS_VERSION      0x0001

#define EEPROM_DISP_S1_SETTINGS_ADDR     0x0700  /* S1 gain/d0/zero_offset (WP10) */
#define EEPROM_DISP_S1_SETTINGS_VERSION  0x0001
#define EEPROM_DISP_S2_SETTINGS_ADDR     0x0800  /* S2 gain/d0/zero_offset (WP10) */
#define EEPROM_DISP_S2_SETTINGS_VERSION  0x0001
#define EEPROM_DISP_SHARED_SETTINGS_ADDR    0x0900  /* atten -- shared, not per-sensor (WP10) */
#define EEPROM_DISP_SHARED_SETTINGS_VERSION 0x0001

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

/* --- BLE / RN4871 (WP5) --- */
#define BLE_DEVICE_NAME                 "InclinationMeter"

/* GATT UUIDs — generated once for this product family. They MUST match
 * whatever LevelApp connects to. The RN4871 expects 32 hex chars (no
 * dashes) for PS,/PC, commands. CMD/DATA derived from service by
 * incrementing the last byte. */
#define BLE_SERVICE_UUID                "6E5D4C3B2A1948079F6E5D4C3B2A1980"
#define BLE_CMD_CHAR_UUID                "6E5D4C3B2A1948079F6E5D4C3B2A1981"
#define BLE_DATA_CHAR_UUID               "6E5D4C3B2A1948079F6E5D4C3B2A1982"

/* RN4871 timeouts (ms) */
#define RN4871_CMD_TIMEOUT_MS           500
#define RN4871_REBOOT_TIMEOUT_MS        2000
#define RN4871_BOOT_TIMEOUT_MS          3000

/* --- Shared byte-stream transport framing (WP5.1) ---
 * Used by any svc_api transport that delivers raw bytes rather than
 * whole frames (BLE transparent UART, wired debug UART) — see
 * Services/svc_api.c's ApiByteReassembler. USB HID delivers whole
 * reports already framed by the endpoint, so it doesn't need this. */
#define API_RX_PACKET_TIMEOUT_MS        100     /* abort partial packet after this */

/* HAL_App/hal_uart.c's per-instance DMA ring buffer (USART6 <-> RN4871,
 * USART3 <-> debug/VCP header). Must be a power of 2; sized to fit the
 * longest single reply plus headroom for burst traffic between
 * scheduler ticks. */
#define HAL_UART_RX_RING_SIZE           256

/* --- AD9833 waveform generator DAC (WP7) ---
 * MCLK is fed from TIM1_CH4/PC11, CubeMX-configured (Core/Src/tim.c) for
 * a fixed 64 MHz / (2 x 6) = 5.3333... MHz square wave — see
 * hal_tim_dac_clock_start(). AD9833's output frequency is
 * FREQREG x MCLK / 2^28 (datasheet "Frequency and Phase Registers"). To
 * land exactly 2048 MCLK cycles per output wave (fOUT = MCLK/2048 ~=
 * 2604.2 Hz), FREQREG = 2^28/2048 = 2^17 = 131072 exactly — no rounding
 * error, which is why 2048 was chosen as the cycles-per-wave target
 * rather than some other divisor. */
#define AD9833_FREQREG                  131072UL   /* = 0x00020000, = 2^17 */

/* --- ADS131M04 simultaneous-sampling ADC (WP8) ---
 * MCLK is fed from TIM2_CH3/PB10, the same 64 MHz / (2 x 6) = 5.3333...
 * MHz square wave as the DAC's MCLK (see hal_tim_adc_clock_start()) — the
 * DAC and ADC deliberately share this exact relationship so the sample
 * rate below is a fixed, known multiple of the DAC's output frequency.
 *
 * ADS131M04's own OSR is defined as fMOD/fDATA, where fMOD = fCLKIN/2
 * (datasheet "OSR Settings and Data Rates"). We want fDATA = fCLKIN/256
 * (a conversion "every 256 [CLKIN] cycles", per spec) = 8 x the DAC's
 * output frequency (2048 MCLK-cycles-per-wave / 256 = 8 samples/cycle
 * exactly). Solving: OSR = fMOD/fDATA = (fCLKIN/2)/(fCLKIN/256) = 128 —
 * register field value 000b (CLOCK register OSR[2:0], datasheet Table
 * 8-17), NOT 256. */
#define ADS131M04_OSR_FIELD             0x0U       /* CLOCK.OSR[2:0] = 000b -> OSR = 128 */

/* Trigger timer (TIM7, no GPIO output — see hal_tim_adc_trigger_start()):
 * interrupts once per ADC sample. 64 MHz / 3072 = 20833.33 Hz exactly
 * (matches fDATA above: fCLKIN/256 = 5,333,333.33/256 = 20833.33 Hz).
 * Prescaler=0, Counter Period=3071 (3072 counts per period). */
#define ADS131M04_TRIGGER_TIMER_PERIOD  3071U

/* --- BME280 environmental sensor (WP9) ---
 * Shares I2C1 with the EEPROM (see pin_config.h) — no CubeMX changes
 * needed, only a different 7-bit address per transaction.
 *
 * Oversampling x1 temperature / x1 pressure / x1 humidity, IIR filter
 * off, forced mode re-triggered once per second by our own scheduler
 * task -- Bosch's own datasheet "humidity sensing" recommended profile
 * (Table 8: forced mode, 1 sample/second, osrs_t=x1/osrs_h=x1) extended
 * to also sample pressure at x1 instead of skipping it. Max conversion
 * time at these settings is ~9.3 ms (datasheet Appendix B, section 9.1
 * formula: t_measure_max = 1.25 + 2.3 + 2.3+0.575 + 2.3+0.575 ~= 9.3 ms),
 * short enough that drv_bme280.c polls the status register with a
 * bounded blocking wait rather than a multi-tick async state machine --
 * NOT the same as drv_24lc256.c's EEPROM write-cycle poll (that one
 * really is non-blocking, spread across scheduler ticks via
 * OP_WRITE_CYCLE_POLL); this is a deliberate blocking tradeoff of its
 * own, justified by the short, tightly-bounded worst case once
 * hal_i2c.c's per-call I2C_TIMEOUT_MS was tightened alongside this
 * driver (a code-review finding — a stuck/disconnected sensor could
 * otherwise stall the whole cooperative scheduler for hundreds of ms). */
#define BME280_CTRL_HUM_VALUE   0x01U   /* osrs_h[2:0] = 001b -> x1 */
#define BME280_CTRL_MEAS_VALUE  0x25U   /* osrs_t=001b, osrs_p=001b, mode=01b (forced) */
#define BME280_CONFIG_VALUE     0x00U   /* t_sb (unused, forced mode), filter off, spi3w_en=0 */

/* Not EEPROM-configurable (DeviceSettings has no room left -- same
 * reasoning as DEFAULT_TASK_UART_MS above) -- fixed literal used directly
 * in App/app_scheduler.c's task table. */
#define DEFAULT_TASK_BME280_MS  1000U

/* --- Differential-capacitor displacement sensors S1/S2 (WP10) ---
 * Replaces Services/svc_signal_analysis.c (WP8's "first cut... additional
 * math may follow" generic 4-channel amplitude/phase diagnostic) --
 * Services/svc_displacement.c is now the sole consumer of
 * drv_ads131m04's per-sample callback. See that file's top comment for
 * the channel mapping (ADS131M04 CH0..CH3 -> B/A/S1/S2) and the full x/
 * delta derivation -- not repeated here to avoid the two drifting apart.
 *
 * G (S-channel amplifier gain) and d0 (neutral air gap) are per-sensor,
 * independently calibratable -- each head's own analog front end and
 * mechanical assembly can differ. atten (A/B attenuation ahead of the
 * ADC) is shared: both heads are driven from the same excitation pair
 * through the same attenuator, so there is exactly one instrument-wide
 * value, not one per sensor. Only the product k = atten*G matters for
 * computing x (see svc_displacement.c) -- G and atten are still stored
 * and calibrated separately, per the task spec, so a future per-sensor
 * front-end change doesn't require re-deriving a combined constant by
 * hand. */
#define DEFAULT_DISP_GAIN          10.0f   /* S-channel amplifier gain, nominal */
#define DEFAULT_DISP_D0_MM         0.1f    /* neutral-position air gap, mm, nominal */
#define DEFAULT_DISP_ZERO_OFFSET_MM 0.0f   /* uncalibrated until an operator zeroes it */
#define DEFAULT_DISP_ATTEN         3.0f    /* A/B attenuation ahead of the ADC, nominal */

/* Per-cycle producer (ISR, raw I/Q) -> consumer (scheduler task, x/delta)
 * ring buffer depth. One carrier cycle = MATH_PHASOR_SAMPLES_PER_CYCLE
 * (8) ADC samples ~= 384 us at ADS131M04_TRIGGER_TIMER_PERIOD's
 * 20833.33 Hz, so 64 entries ~= 24.6 ms of headroom -- comfortably
 * covers WP9's now-tightened worst-case scheduler stall (~10-20 ms) with
 * margin. Must be a power of two (bitmask index wrap, CLAUDE.md 8.3).
 * Same depth reused for the output (delta1/delta2/residual) ring buffer
 * -- both are drained every scheduler tick, so there's no reason for one
 * to run deeper than the other. */
#define DISPLACEMENT_RING_DEPTH    64U

#endif /* CONFIG_H */
