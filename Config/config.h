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

/* --- ADS131M04 simultaneous-sampling ADC (WP8) ---
 * MCLK is fed from TIM2_CH3/PB10, the same 64 MHz / (2 x 6) = 5.3333...
 * MHz square wave as the DAC's MCLK (hal_tim_adc_clock_start()) — the DAC
 * and ADC deliberately share this exact clock so the sample rate below is
 * a fixed, known multiple of the DAC's output frequency.
 *
 * ADS131M04 OSR = fMOD/fDATA, fMOD = fCLKIN/2 (datasheet "OSR Settings
 * and Data Rates"). We want fDATA = fCLKIN/256 = 8 x the DAC output
 * frequency (2048 MCLK-cycles-per-wave / 256 = 8 samples/cycle). So
 * OSR = (fCLKIN/2)/(fCLKIN/256) = 128 -> CLOCK.OSR[2:0] field = 000b. */
#define ADS131M04_OSR_FIELD            0x0U       /* CLOCK.OSR[2:0] = 000b -> OSR = 128 */

/* DRDY poll timer (TIM7, no GPIO output). Originally one tick per ADC
 * sample (20833.33 Hz), but TIM7 and the ADS's own fDATA are two
 * independent 20833 Hz clocks with a drifting phase relationship — when a
 * tick repeatedly lands just before DRDY, two conversions pass between
 * reads and the capture decimates non-uniformly (bench: effective rate
 * wandered 8-14 kHz, 2000-6700 "drops"). Fix: oversample. At 2x =
 * 41666.67 Hz (64 MHz / 1536, Prescaler=0, Period=1535) every DRDY-low is
 * serviced within ~24 us, inside the 48 us conversion period, so exactly
 * one read happens per conversion — uniform sampling at the true fDATA.
 * Ticks that find DRDY already high are benign no-ops now, not missed
 * samples. The TIM7 ISR uses a lean fast path (Core/Src/stm32g0xx_it.c)
 * rather than the full HAL_TIM_IRQHandler, which is too heavy at this rate. */
#define ADS131M04_TRIGGER_TIMER_PERIOD 1535U

/* Services/svc_signal_analysis.c: complete 8-sample sine cycles per
 * amplitude/phase recompute. 64 cycles = 512 samples ~= 24.6 ms at
 * 20833.33 Hz — first-cut update rate, safe to retune. */
#define SIGNAL_ANALYSIS_BATCH_CYCLES   64U

/* --- Bulk raw-ADC capture (API v2, category 0x8 / START_BULK) ---
 * Decouples high-rate sampling from transport speed: the device fills a
 * RAM buffer at the full 20833.33 Hz sample rate, then streams it out in
 * chunks over whatever transport at whatever speed the link allows
 * (docs/api-v2-spec.md §4.5).
 *
 * Buffer = ADC_BULK_SAMPLE_COUNT samples x 4 channels x 3 bytes. The ADC
 * codes are 24-bit, stored packed little-endian signed -- the
 * sign-extension byte that int32 storage wasted is dropped. 6144 x 4 x 3
 * = 73728 bytes ~= 50% of the 144 KB SRAM. At 20833.33 Hz one capture
 * spans ~295 ms (~768 cycles of the 2604 Hz DAC tone). */
#define ADC_BULK_SAMPLE_COUNT          6144U

/* 4 channels x 3 bytes -- size of one full sample, in RAM and on the wire. */
#define ADC_BULK_BYTES_PER_SAMPLE     12U

/* Samples per bulk chunk packet. Each chunk payload is
 * [page:1][sample:12]xN; the whole API2 packet must fit API2_PACKET_MAX_SIZE
 * (128): 6 (frame) + 1 (status) + 1 (page) + 12*N <= 128 -> N <= 10. */
#define ADC_BULK_CHUNK_SAMPLES        10U

/* Chunks pushed per svc_api_update() tick, upper bound — actual pace is
 * governed by the transport TX-ring headroom (svc_api's ready_fn). Keeps
 * the pump yielding so command responses / other traffic still get a turn
 * mid-transfer (docs/api-v2-spec.md §4.1). */
#define ADC_BULK_CHUNKS_PER_TICK      4U

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

#endif /* CONFIG_H */
