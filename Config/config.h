#ifndef CONFIG_H
#define CONFIG_H

/* --- System tick --- */
#define SYSTICK_RATE_HZ                 1000
#define SYSTICK_PERIOD_MS               (1000 / SYSTICK_RATE_HZ)

/* --- Scheduler task periods (ms) --- */
#define DEFAULT_TASK_SENSORS_MS         100
#define DEFAULT_TASK_LED_MS             250     /* status LED toggle period -> 2 Hz blink */
#define DEFAULT_TASK_BLE_MS             100
#define DEFAULT_TASK_USB_MS             100
#define DEFAULT_TASK_BATTERY_MS         1000
#define DEFAULT_TASK_TEMPERATURE_MS     10000

/* --- Display page rendering ---
 * app_display_update() renders the frame in u8g2 page-buffer mode: 15
 * bands (2 tile-rows / 16 px each) into drv_sharp_lcd's framebuffer,
 * then one DMA blit. task_display runs every tick and advances at most
 * this many bands per call, so no single call blocks the cooperative
 * scheduler with a full 400x240 render (~100 ms at -O0, still tens of ms
 * at -O2). 3 bands/tick -> a full redraw completes in ~5 scheduler ticks;
 * on a Sharp LCD the top-to-bottom fill is imperceptible.
 *
 * Event-driven redraws: a frame started by a user input (urgent) renders
 * DISPLAY_URGENT_PAGES_PER_TICK bands per tick so the response is as fast as
 * possible (and is restarted if another input arrives meanwhile); a
 * periodic frame (new measurement, clock, battery) renders only the bands
 * whose content changed, DISPLAY_PAGES_PER_TICK per tick, so it never
 * starves the measurement pipeline. */
#define DISPLAY_PAGES_PER_TICK          3
#define DISPLAY_URGENT_PAGES_PER_TICK   5

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
/* R6/R9 battery divider (BATTERY_SENSE, pin_config.h) swapped on the
 * board 2026-09-24: was 100k/33k (ratio 33/133 = 0.248, 4.2V -> 1.04V),
 * now 33k/100k (ratio 100/133 = 0.752, 4.2V -> 3.16V) -- the ADC now
 * reads ~3.03x (100/33) more signal for the same battery voltage. NUM
 * stays 133; only DEN changes (Vbat_mv = V_ADC_mv * NUM/DEN + offset,
 * svc_battery.c). */
#define DEFAULT_VBAT_SCALE_NUM          133
#define DEFAULT_VBAT_SCALE_DEN          100
#define DEFAULT_VBAT_OFFSET_MV         0       /* additive Vbat correction; set by bench cal --
                                                  * reset to 0 with the divider swap above; the
                                                  * old +79mV (memory: battery-voltage-calibrated)
                                                  * was fit to the old ratio's residual error and
                                                  * does not carry over. Needs a fresh bench cal. */
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

/* --- RTC trim (2026-10-08) ---
 * Clock-rate correction in 0.1 ppm, positive = faster. 0 = the crystal as it is. Calibrate by measuring the drift
 * against a reference over several hours (API Calibrations RTC_TRIM, System RTC has a 1/256 s counter). */
#define DEFAULT_RTC_TRIM_PPM_X10  0

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

#define EEPROM_SCHEDULER_SETTINGS_ADDR    0x0000  /* task periods */
#define EEPROM_SCHEDULER_SETTINGS_VERSION 0x0003  /* 0x0003: dropped task_display_ms (the display task runs every
                                                     tick; the setting paced nothing). 0x0002: dropped the REV A
                                                     stream_interval / settling /
                                                     complementary-filter / task_processing
                                                     fields — a stored 0x0001 page is
                                                     discarded and reseeded from DEFAULT_*. */
#define EEPROM_BATTERY_SETTINGS_ADDR      0x0100  /* thresholds + ADC divider scale */
#define EEPROM_BATTERY_SETTINGS_VERSION   0x0005  /* 0x0002: added battery_charge_start_mv
                                                     and retuned thresholds (WP2 debug).
                                                     0x0003 (WP6): the page's alignment pad
                                                     became auto_poweroff_s.
                                                     0x0004: added vbat_offset_mv (bench
                                                     calibration).
                                                     0x0005 (2026-09-26): the R6/R9 divider
                                                     swap (2026-09-24, this file's
                                                     DEFAULT_VBAT_SCALE_NUM/DEN comment)
                                                     changed the DEFAULT_* scale values but
                                                     was never version-bumped, so boards
                                                     already provisioned before the swap kept
                                                     the stale pre-swap vbat_scale_den in
                                                     EEPROM forever -- reported as an
                                                     obviously-wrong ~11.8V battery reading
                                                     (real value ~3.9V) on bench board 2.
                                                     A stored older page is discarded and
                                                     reseeded from DEFAULT_* (which hold the
                                                     tuned thresholds and the correct
                                                     post-swap scale). */
#define EEPROM_TMP236_SETTINGS_ADDR       0x0200  /* on-board temp sensor piecewise-linear constants */
#define EEPROM_TMP236_SETTINGS_VERSION    0x0001
#define EEPROM_LM35_SETTINGS_ADDR         0x0300  /* external temp sensor scale */
#define EEPROM_LM35_SETTINGS_VERSION      0x0001
#define EEPROM_ENCODER_SETTINGS_ADDR      0x0400  /* quadrature counts/detent */
#define EEPROM_ENCODER_SETTINGS_VERSION   0x0001
#define EEPROM_RTC_SETTINGS_ADDR          0x0600  /* RTC crystal trim */
#define EEPROM_RTC_SETTINGS_VERSION       0x0001
#define EEPROM_DISPLACEMENT_SETTINGS_ADDR    0x0500  /* WP10 disp calibration —
                                                         was the REV A SCL3300/
                                                         PCAP04 tilt CalibrationData
                                                         page, removed with the
                                                         rest of the REV A sensor
                                                         stack; first REV B use of
                                                         this freed page. */
#define EEPROM_DISPLACEMENT_SETTINGS_VERSION 0x000C  /* bump on ANY layout/default change of the page (an old
                                                         page is then reseeded to defaults and a WARN is
                                                         logged); version history in docs/decisions.md */

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
#define USB_HID_REPORT_SIZE             64
#define USB_PRODUCT_STR                 "InclinationMeter"
/* No USB_SERIAL_STR here anymore — the API v2 IDENTITY response's
 * serial_str is derived per-board from the MCU's factory UID
 * (HAL_App/hal_mcu.c), not a fixed string. The USB device descriptor's own
 * iSerialNumber is separate and already UID-derived by CubeMX's generated
 * Get_SerialNum() (USB_Device/App/usbd_desc.c). */

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

/* DRDY poll timer (TIM7, no GPIO output). fDATA = SYSCLK / 3072 exactly
 * (64 MHz / 12 / 256 — the MCLK divisor x OSR), so one fDATA period is
 * 3072 TIM7 ticks. TIM7 and fDATA are therefore frequency-LOCKED, not
 * two free-running clocks — the phase relationship is fixed.
 *
 * ADC_TRIGGER_OVERSAMPLE = how many TIM7 fires per conversion. A polled
 * read needs at least one fire safely inside every conversion period;
 * 2x provides a spare so a late fire (higher-priority IRQ) still catches
 * the conversion before the ADS overwrites it.
 *
 * 1x was re-tried on the clean Phase 1/2 read path (fw 0.9.27/0.9.29,
 * with the SysTick-referenced integrity check watching): NOT reliable.
 * frequency-lock does not save it — the startup phase between TIM7 and
 * the ADS conversion is random per start(), and an unfavourable one puts
 * every poll on the DRDY edge with zero read margin. Observed: mostly a
 * slow ~0.5 lost-frame/s drift, occasionally catastrophic (~120/s,
 * ADS_FAULT_SLIP within ~15 s). 2x holds frame_deficit at 0 indefinitely.
 * Keep 2x. The only way to cut the ISR cost further is a timer->DMA
 * read with no per-sample CPU interrupt (docs/adc_acquisition_redesign.md).
 *
 * The TIM7 ISR uses a lean fast path (Core/Src/stm32g0xx_it.c) rather
 * than the full HAL_TIM_IRQHandler. */
#define ADS131M04_FDATA_TIMER_TICKS   3072U
#define ADC_TRIGGER_OVERSAMPLE        2U
#define ADS131M04_TRIGGER_TIMER_PERIOD \
    ((ADS131M04_FDATA_TIMER_TICKS / ADC_TRIGGER_OVERSAMPLE) - 1U)

/* Expected value of every streaming frame's word 0 (the ADS131M04 STATUS
 * response when no command was issued): WLENGTH=24-bit, all DRDY set, no
 * RESET / F_RESYNC / CRC_ERR. Any other value == the frame stream lost
 * byte alignment, or the device resynced/reset. Confirmed on the bench
 * (fw 0.9.17): constant 0x010F. */
#define ADS131M04_STATUS_WORD         0x010FU

/* Conversion-count integrity. frames_produced must track elapsed time x
 * fDATA. fDATA = SYSCLK / ADS131M04_FDATA_TIMER_TICKS exactly, and SysTick
 * shares the SYSCLK root, so per elapsed millisecond the expected frame
 * count is ADC_FDATA_KHZ_NUM / ADC_FDATA_KHZ_DEN with no drift. (tim7_fires
 * is NOT the reference — the trigger ISR can miss a fire under load,
 * ~20 ppm, without any sample being lost: bench fw 0.9.24, frame rate came
 * out +0.9 ppm while fire rate was -21 ppm.)
 *   deficit = expected_frames(now) - frames_produced
 * A real lost or duplicated conversion is a permanent +/-1 step; the
 * measured noise of this estimate is +/-2 over 90 s. deficit past
 * ADC_FRAME_DEFICIT_LIMIT and staying there ADC_FRAME_DEFICIT_HOLD_MS
 * latches ADS_FAULT_SLIP. */
#define ADC_FDATA_KHZ_NUM            64000U    /* 64 MHz / 3072 -> frames per ms */
#define ADC_FDATA_KHZ_DEN           ADS131M04_FDATA_TIMER_TICKS
#define ADC_SLIP_SETTLE_MS          2000U
#define ADC_FRAME_DEFICIT_LIMIT     8         /* frames off the SysTick estimate */
#define ADC_FRAME_DEFICIT_HOLD_MS   200U      /* sustained before it latches */

/* --- ADS131M04 frame ring (docs/adc_acquisition_redesign.md) ---
 * The SPI1 RX DMA writes each frame straight into a ring slot; the TIM7
 * ISR only advances the head index. The per-sample sign-extend runs in
 * the SysTick drain (drv_ads131m04_drain_tick, ~21 frames/ms at fDATA),
 * which calls into Services/svc_displacement.c's per-sample callback.
 * Ring depth in frames (power of two — index math uses & (N-1)). 64 x
 * 18 B ~= 1.2 KB, ~2 ms of slack before the DMA laps the drain. Producer
 * laps consumer -> ring_overflow counter + drop-newest (Phase 2 adds a
 * latching ERROR). */
#define ADC_FRAME_RING_FRAMES          64U

/* Scheduler-gap diagnostic threshold (2026-09-26, added to investigate an
 * observed batch-throughput shortfall -- docs/wp10_displacement.md has
 * the full writeup). svc_displacement_update()'s "how many calls saw a
 * gap this big since the last one" counter, Services/svc_displacement.c's
 * s_gap_over_threshold_count. Chosen well below the batch ring's slack
 * ((DISPLACEMENT_BATCH_RING_DEPTH - 1) x 24.6 ms) -- "close enough to start
 * threatening the ring, whether or not it actually overflowed this specific
 * time." Purely a diagnostic knob, not a behavioral one -- changing it
 * doesn't affect drops themselves, only how the counter buckets them. */
#define DISPLACEMENT_GAP_WARN_THRESHOLD_MS  40U

/* Cycles summed per batch (one batch = 64 x 384 us = 24.6 ms, 40.7 batches/s).
 * The expensive per-batch complex division (soft-float, no FPU) runs once per
 * batch, so this sets the CPU load; the per-cycle version livelocked the
 * scheduler. Bounded draining (DISPLACEMENT_MAX_BATCHES_PER_TICK) turns any
 * future overload into dropped batches, never a livelock. 64 is the value the
 * quality windows (Math/math_window.h) and the Hann filters are designed
 * around. History of the 32 -> 128 -> 64 changes, the livelock root cause and
 * the board-2 margin findings: docs/decisions.md. */
#define DISPLACEMENT_BATCH_CYCLES          64U

/* --- Per-position batch accumulation (2026-10-03) ---
 * The demodulation hot path no longer does a weighted 64-bit multiply-
 * accumulate per sample (8 library calls to __aeabi_lmul per ADC sample on
 * the FPU-less, 64-bit-multiply-less Cortex-M0+: ~20% of the core). Instead
 * on_sample() adds each sample into a plain int32 sum for its position in
 * the 8-sample cycle (s[channel][n] += code, one add, no sign logic), over
 * the whole DISPLACEMENT_BATCH_CYCLES-cycle batch; the 14-bit DFT weights are
 * applied once per batch by math_phasor_combine() (I = 16384*(s0-s4) +
 * 11585*(s1-s3-s5+s7), Q likewise) -- exactly the same integers as before.
 * Each position sum adds BATCH_CYCLES signed 24-bit codes: |sum| <=
 * BATCH_CYCLES * 2^23, which fits int32 for BATCH_CYCLES <= 256 (static-
 * asserted in svc_displacement.c). The producer->consumer ring therefore
 * carries one entry per completed BATCH (4 channels x 8 sums x int32 + seq =
 * 130 B), not one per cycle:
 *   DISPLACEMENT_BATCH_RING_DEPTH -- batches of slack between the sample
 *       callback and svc_displacement_update() (power of two). 16 entries, 15
 *       usable = ~370 ms (it was 4: 3 usable, ~74 ms, which a 190 ms display
 *       redraw overran, and every dropped batch breaks the Hann and quality
 *       windows) for 2.1 KB of RAM (the original per-cycle ring was 128 x 66 B
 *       = 8.4 KB). If the task falls
 *       further behind, the whole newest batch is dropped (input_drop_count
 *       += BATCH_CYCLES; the seq counter still advances, so a gap shows up as
 *       a seq jump of a multiple of BATCH_CYCLES) -- there are no partial
 *       batches any more.
 *   DISPLACEMENT_MAX_BATCHES_PER_TICK -- the bounded-drain cap that replaces
 *       DISPLACEMENT_MAX_CYCLES_PER_TICK (a cap exists so an overload drops
 *       batches instead of livelocking the scheduler, docs/decisions.md): batches processed per svc_displacement_update()
 *       call. Each costs a few thousand soft-float cycles, so a transient
 *       backlog degrades to dropped batches instead of ever livelocking the
 *       scheduler. */
#define DISPLACEMENT_BATCH_RING_DEPTH     16U
#define DISPLACEMENT_MAX_BATCHES_PER_TICK 2U

/* Display stream (2026-10-05, window changed 2026-10-07): the LIVE screen does
 * not show the ~40.7 Hz batch stream but a condensed ~4 Hz reading (the
 * API's polled/subscribed delta values are this same stream since 2026-10-07;
 * the raw batch value is Topic 0x03): a Hann
 * window over the newest DISPLACEMENT_DISPLAY_TAPS contiguous batch readings,
 * published every DISPLACEMENT_DISPLAY_DECIMATION-th batch (40.7 / 10 = 4.07 Hz).
 * 25 taps = 0.61 s, delay ~0.3 s, -3 dB at 1.1 Hz, and at least -42 dB over
 * 5 to 20 Hz (worst case per band: -42 dB at 5-10 Hz, -60 dB at 10-15 Hz,
 * -73 dB at 15-20 Hz), which covers the ~20 Hz sensor resonance wherever it
 * sits (18 - 21.5 Hz seen) -- a plain boxcar or the earlier triangular window
 * (two 10-batch boxcars, nulls only at multiples of 4.07 Hz) rejected less
 * away from 20.35 Hz. Chosen with the 19 h contiguous phasor stream
 * (Testing/2026-09-30_bulk_adc_30s_interval/findings.md, section 8 and the
 * display-filter comparison): the scatter of the 4 Hz output fell 2.6x (night) to 5x (day) against a
 * plain average of 10 and a further 13-30% against triangular-19. */
#define DISPLACEMENT_DISPLAY_DECIMATION    10U
#define DISPLACEMENT_DISPLAY_TAPS          25U

/* Window-level quality indicator (2026-10-07), used by the LIVE display
 * ("!" next to a doubtful reading) and by the precision measurement below.
 * The mean squared batch-to-batch step of Im(x) over a window (Math/math_window.h)
 * is compared with the instrument's own quiet floor -- the lowest such value
 * over the last while, allowed to creep up so it doubles in about
 * DISPLACEMENT_QUALITY_FLOOR_DOUBLING_S if every window is noisier; a window is
 * "doubtful" when its mean step power exceeds DISPLACEMENT_QUALITY_K x floor.
 * K = 6 (power, ~2.4x in amplitude): on the 19 h stream this flags 0.8% of
 * display readings at night and 8% by day, and 1% of 2 s precision attempts
 * fail. The floor needs two independent 2 s windows after a start before it is
 * trusted (math_window_floor_ready); until then nothing is flagged. */
#define DISPLACEMENT_QUALITY_K                 6.0f
#define DISPLACEMENT_QUALITY_FLOOR_DOUBLING_S  1200U

/* If a sensor's windows stay flagged this long (10 minutes) the quiet level has
 * really changed -- a frozen or dead channel came back, a sensor was swapped --
 * and the floor is re-seeded from the current window instead of staying stuck
 * near zero for hours (Math/math_window.h). Shorter than that, a disturbance
 * can not raise its own limit. */
#define DISPLACEMENT_QUALITY_RESEED_S          600U

/* Tilt calibration seeds (2026-10-06 redesign, system_state.h has the
 * model): tilt [mm/m] = (r - zero) / k, r = Re[S/(PGA*D)*e^{-j delta}].
 * k is stored x1e-6 and stated for PGA = 1. Default 0.0213 per (mm/m) =
 * the nominal value: Wyler 20 uV RMS per um/m at 2 V RMS excitation
 * (S/A = 1e-5 per um/m, S/D = 0.5e-5), times the ratio of the S-path to the
 * A/B-path gain in the ADC domain (0.903 / 0.212 = 4.26 at PGA 1, REV B
 * networks): 4.26 * 0.5e-5 * 1000 = 0.0213 (docs/signal_processing.tex
 * Sec. 13.3). A real instrument differs (the 2026-09-29 calibrations
 * correspond to about 0.013 at PGA 1) -- calibrate k against a known tilt.
 * zero starts at 0 ppm (no flip calibration yet). */
#define DEFAULT_DISP_S1_K_MICRO            21300
#define DEFAULT_DISP_S1_ZERO_PPM               0
#define DEFAULT_DISP_S2_K_MICRO            21300
#define DEFAULT_DISP_S2_ZERO_PPM               0
#define DEFAULT_DISP_S1_INVERT                 0    /* 0=normal, 1=inverted -- see system_state.h */
#define DEFAULT_DISP_S2_INVERT                 0
/* Phase calibration defaults, centidegrees (docs/signal_processing.tex
 * Sec. 9.1): principal axis of S/D over the 19 h phasor stream of
 * 2026-10-04/05 (S1 -6.0 deg, S2 -9.15 deg). Estimates only -- determine
 * them properly from a tilt change (Sec. 9.1) once the instrument is
 * calibrated, via API Calibrations 0x0D / 0x0E. */
#define DEFAULT_DISP_S1_PHASE_CDEG             (-600)
#define DEFAULT_DISP_S2_PHASE_CDEG             (-915)

/* Zero clamp, ppm of the ratio r (system_state.h): +-0.5 is far wider than
 * any real zero error (10 mm/m at k = 0.0213 is 0.21), tight enough to catch
 * a nonsense value. Shared by svc_api.c's SF() bounds and
 * zero_cal_apply_if_ready()'s clamp so they cannot drift apart. */
#define DISPLACEMENT_ZERO_PPM_MAX          500000

/* Wyler handbook constant: 20uV RMS (at the sensor's own S-channel output,
 * referred to the ADC pin -- i.e. AFTER that channel's own PGA, same
 * reference point svc_displacement_get_signal_diag() reports) per um/m of
 * tilt. Used for the
 * DIAGNOSTICS screen / API's live "theoretical tilt" readout, so a user
 * doing a real reference-tilt calibration can compare it side by side with
 * the device's own delta_mm without needing a calculator. */
#define DISPLACEMENT_WYLER_UV_RMS_PER_UM_PER_M   20U

/* --- Flip (zero) calibration ---
 * 180-degree reversal test: average the reading in two orientations; a real
 * surface tilt flips sign, the instrument's zero error does not, so
 * zero = (step1 + step2) / 2 (docs/signal_processing.tex, Sec. 13.7). Per
 * sensor, per instrument (stored in g_device_settings, ppm of the ratio r).
 * DISPLACEMENT_ZERO_CAL_SAMPLES = batches averaged per step: 64 batches =
 * ~1.6 s, a total depth of 64 x 64 = 4096 raw cycles (it follows
 * DISPLACEMENT_BATCH_CYCLES so the wall-clock time per step stays ~1.6 s). */
#define DISPLACEMENT_ZERO_CAL_SAMPLES        64U

/* --- Triggered precision measurement (2026-09-26, redesigned 2026-10-07) ---
 * the API/UI-triggered "take a reliable reading" mode (Services/svc_api.c's
 * Commands API2_RES_CMD_PRECISION_MEASURE, right knob on LIVE). A sliding
 * window of DISPLACEMENT_PRECISION_WINDOW_BATCHES contiguous batches (81 =
 * 1.99 s) is weighted with a Hann window; as soon as a window that lies
 * entirely after the trigger is CLEAN (the window-level indicator above is
 * <= K x floor for BOTH sensors) its Hann-weighted means (S1, S2 and the
 * differential S1-S2) are the result. If no clean window exists within
 * DISPLACEMENT_PRECISION_TIMEOUT_MS the measurement fails with an error (no
 * value). Replaces the earlier "average 64 quality-flagged batches" scheme:
 * dropping individual batches breaks the cancellation of the ~20 Hz
 * resonance (a Nyquist-folded alternation), a window gate does not. On the
 * 19 h contiguous stream this gave a repeatability of 0.023 um at night and
 * 0.025 um by day (nominal units), against 0.078 um by day without the gate,
 * 1% failures, 2.02 s mean duration. A window longer than 2 s did not help
 * (night -20% for 2.5x the time, day nothing): the daytime excess is rare
 * disturbed windows, which the gate removes, not averaging noise. */
#define DISPLACEMENT_PRECISION_WINDOW_BATCHES 81U
#define DISPLACEMENT_PRECISION_TIMEOUT_MS     5000U

/* --- Bulk raw-ADC capture (API v2 category 0x8: START_BULK/CANCEL_BULK) ---
 * Restored 2026-09-25 -- an important bench diagnostic tool, mistakenly
 * retired 2026-09-24 on the theory that WP10's displacement demod owning
 * the ADS131M04's one sample-callback slot (Services/svc_displacement.c)
 * meant the two couldn't coexist. They can: svc_displacement.c's
 * on_sample() now has a capture-mode branch (ported near-verbatim from
 * the WP8-era Services/svc_signal_analysis.c this replaced) that stores
 * raw codes into this buffer and skips the phasor math entirely while a
 * capture is armed -- same mutual-exclusion shape as before, just living
 * in the new file. Buffer = ADC_BULK_SAMPLE_COUNT samples x 4 channels x
 * 3 bytes. The ADC codes are 24-bit, stored packed little-endian signed --
 * the sign-extension byte that int32 storage wasted is dropped. 6144 x 4
 * x 3 = 73728 bytes ~= 50% of the 144 KB SRAM. At 20833.33 Hz one capture
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

/* --- Displacement phasor diagnostics ---
 * Exposes the demod's intermediate I/Q phasors (Services/svc_displacement.c's
 * BatchSums -- the batch-summed values feeding process_one_batch()'s complex
 * division, one step upstream of delta_mm/residual) for bench diagnosis.
 * Real-time snapshot: API v2 Topic 0x5 / resource 0x02 (latest batch, 8 floats,
 * GET + SUBSCRIBE; an interval snapshot, so it aliases the 20 Hz sensor
 * resonance -- not for analysis). Gapless: the continuous stream below. (The
 * one-shot "bulk phasor log", Bulk 0x8 / 0x01, was removed 2026-10-07: the
 * stream replaced it and its 17 KB buffer was the biggest dead-weight RAM user.) */
/* Continuous phasor batch stream FIFO (API Topic 0x5 / res 0x05, added
 * 2026-10-04): 64 entries x 34 B = 2176 B, ~1.57 s of batches at 40.7 Hz.
 * It only has to ride out main-loop stalls and UART back-pressure (the
 * transport drains ~11 kB/s against a ~1.75 kB/s stream); a LIVE-screen
 * redraw stalls the loop for ~8 batches. Power of two (index mask). */
#define DISPLACEMENT_PHASOR_STREAM_DEPTH       64U

/* Stream frames handed to the transport per svc_api_update() tick, upper
 * bound (the transport's ready hook limits it further). */
#define DISPLACEMENT_PHASOR_STREAM_PER_TICK    4U

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
 * otherwise stall the whole cooperative scheduler for hundreds of ms).
 *
 * BME280_UPDATE_BUDGET_MS is a hard ceiling on the wall time one
 * drv_bme280_update() call may spend, checked at every phase boundary so
 * the stall is provably bounded regardless of the failure mode (clean
 * NAK, clock-stretch, or a wedged bus that only hal_i2c's own timeout
 * would otherwise unstick). 15 ms leaves margin over the ~9.3 ms healthy
 * conversion plus a few short I2C transactions. */
#define BME280_UPDATE_BUDGET_MS  15U

#define BME280_CTRL_HUM_VALUE   0x01U   /* osrs_h[2:0] = 001b -> x1 */
#define BME280_CTRL_MEAS_VALUE  0x25U   /* osrs_t=001b, osrs_p=001b, mode=01b (forced) */
#define BME280_CONFIG_VALUE     0x00U   /* t_sb (unused, forced mode), filter off, spi3w_en=0 */

/* Not EEPROM-configurable (DeviceSettings has no room left -- same
 * reasoning as DEFAULT_TASK_UART_MS above) -- fixed literal used directly
 * in App/app_scheduler.c's task table. */
#define DEFAULT_TASK_BME280_MS  1000U

#endif /* CONFIG_H */
