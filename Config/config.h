#ifndef CONFIG_H
#define CONFIG_H

/* --- System tick --- */
#define SYSTICK_RATE_HZ                 1000
#define SYSTICK_PERIOD_MS               (1000 / SYSTICK_RATE_HZ)

/* --- Scheduler task periods (ms) --- */
#define DEFAULT_TASK_SENSORS_MS         100
#define DEFAULT_TASK_DISPLAY_MS         100
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
 * on a Sharp LCD the top-to-bottom fill is imperceptible. */
#define DISPLAY_PAGES_PER_TICK          3

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

#define EEPROM_SCHEDULER_SETTINGS_ADDR    0x0000  /* task periods */
#define EEPROM_SCHEDULER_SETTINGS_VERSION 0x0002  /* 0x0002: dropped the REV A
                                                     stream_interval / settling /
                                                     complementary-filter / task_processing
                                                     fields — a stored 0x0001 page is
                                                     discarded and reseeded from DEFAULT_*. */
#define EEPROM_BATTERY_SETTINGS_ADDR      0x0100  /* thresholds + ADC divider scale */
#define EEPROM_BATTERY_SETTINGS_VERSION   0x0004  /* 0x0002: added battery_charge_start_mv
                                                     and retuned thresholds (WP2 debug).
                                                     0x0003 (WP6): the page's alignment pad
                                                     became auto_poweroff_s.
                                                     0x0004: added vbat_offset_mv (bench
                                                     calibration). A stored older page is
                                                     discarded and reseeded from DEFAULT_*
                                                     (which hold the tuned thresholds). */
#define EEPROM_TMP236_SETTINGS_ADDR       0x0200  /* on-board temp sensor piecewise-linear constants */
#define EEPROM_TMP236_SETTINGS_VERSION    0x0001
#define EEPROM_LM35_SETTINGS_ADDR         0x0300  /* external temp sensor scale */
#define EEPROM_LM35_SETTINGS_VERSION      0x0001
#define EEPROM_ENCODER_SETTINGS_ADDR      0x0400  /* quadrature counts/detent */
#define EEPROM_ENCODER_SETTINGS_VERSION   0x0001
#define EEPROM_DISPLACEMENT_SETTINGS_ADDR    0x0500  /* WP10 disp calibration —
                                                         was the REV A SCL3300/
                                                         PCAP04 tilt CalibrationData
                                                         page, removed with the
                                                         rest of the REV A sensor
                                                         stack; first REV B use of
                                                         this freed page. */
#define EEPROM_DISPLACEMENT_SETTINGS_VERSION 0x0001

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

/* --- Displacement demodulation (WP10, Services/svc_displacement.c) ---
 * Producer (the per-sample callback, called from the SysTick frame
 * drain above) -> consumer (svc_displacement_update(), every scheduler
 * tick) ring depth, one entry per completed 8-sample carrier cycle
 * (~2.6 kHz production rate — see math_phasor.h). Same
 * shape/reasoning as ADC_FRAME_RING_FRAMES above, just one layer up the
 * pipeline (cycles, not raw frames) and used for two SPSC rings (raw
 * I/Q in, computed delta out — see svc_displacement.c). Power of two,
 * index math uses & (N-1). Doubled from 64 to 128 (~49.2 ms of slack)
 * 2026-09-25 as part of the general margin-hardening below -- see that
 * comment for why. */
#define DISPLACEMENT_RING_DEPTH        128U

/* ROOT-CAUSED 2026-09-24 with a real debugger session (STM32_Programmer_CLI
 * -halt/-coreReg/-r32 over SWD -- see docs/wp10_displacement.md for the
 * full walkthrough), after the earlier "known open issue" writeup that
 * used to sit here turned out to be chasing the wrong layer entirely.
 * The MCU was never actually crashed: uwTick (pure SysTick-ISR-driven,
 * independent of the scheduler) kept advancing normally throughout,
 * while app_scheduler.c's s_tasks[] showed task_displacement's
 * last_run_ms frozen from the moment it was first called -- i.e. the
 * scheduler's main loop was stuck *inside one call* to
 * svc_displacement_update(), specifically its `while (s_in_tail !=
 * s_in_head)` drain loop, and never returned.
 *
 * Why: the per-cycle complex-division math (3 float divisions +
 * surrounding multiplies, no hardware FPU on this Cortex-M0+) costs more
 * per cycle than the ~384 us a cycle takes to produce (2.6 kHz carrier).
 * Compute-only, that's already marginal -- bench-measured ~18% of
 * cycles dropped even with nowhere to store the result. Add the few
 * extra stores push_output()/the API snapshot need and the average
 * tips over budget: on_sample() (ISR context) queues new cycles into
 * s_in_ring faster than the drain loop can empty it, so the loop's own
 * exit condition can never become true -- a livelock, not a crash, and
 * not fixable by changing what gets stored (every earlier bisection
 * attempt that "fixed" it by removing storage was really just removing
 * enough per-cycle cost to stay under budget, not fixing a logic bug).
 *
 * Real fix, two parts:
 *  1. DISPLACEMENT_BATCH_CYCLES -- coherently sum this many consecutive
 *     cycles' raw I/Q (cheap int64 adds, same accumulation ISR-side)
 *     before running the expensive per-batch complex division once,
 *     instead of once per single cycle. Cuts the division-heavy work by
 *     this factor (and, as a bonus, is a longer coherent integration --
 *     better SNR, not just a workaround).
 *  2. DISPLACEMENT_MAX_CYCLES_PER_TICK -- defensive cap on how many raw
 *     cycles svc_displacement_update() will dequeue in one call,
 *     regardless of backlog, so a future transient overload (scheduler
 *     jitter, a slow tick elsewhere) can degrade to dropped cycles
 *     (already-proven-safe, graceful) instead of ever livelocking the
 *     scheduler again -- same bounded-pump shape as
 *     DISPLAY_PAGES_PER_TICK / the retired bulk-chunk pump.
 *
 * MARGIN HARDENED 2026-09-25: 8/32 gave board 1 a clean 242 s zero-drop
 * soak, but board 2 -- identical hardware, ordinary component/clock
 * tolerance, no defect found or expected -- measurably drops cycles in
 * bursts at the same settings (confirmed even on firmware predating any
 * of that day's other changes; see docs/wp10_displacement.md's "Board 2's
 * timing margin" section). Two boards behaving differently at a setting
 * this close to its own documented ~18%-marginal budget means the
 * budget was too thin to begin with, not that board 2 is faulty -- the
 * fix is the same lever that solved the original livelock, pushed
 * further: quadrupling DISPLACEMENT_BATCH_CYCLES to 32 cuts the
 * division-heavy work rate to a quarter (~81 updates/s, still ample for
 * a mechanical displacement reading -- WP8's old signal-analysis module
 * ran its own per-batch finalize at just 40 Hz), and doubling
 * DISPLACEMENT_MAX_CYCLES_PER_TICK alongside the doubled
 * DISPLACEMENT_RING_DEPTH above keeps recovery-from-backlog just as fast
 * proportionally. Needs the same re-verification the original fix got
 * (a long soak watching input_drop_count) before being trusted as
 * "enough" margin, not just "more" margin. */
#define DISPLACEMENT_BATCH_CYCLES         32U
#define DISPLACEMENT_MAX_CYCLES_PER_TICK  64U

/* Nominal calibration seeds (DeviceSettings' displacement page, EEPROM-
 * backed past first boot — see system_state.h's comment on those
 * fields). Scaled integers, not raw floats, to fit svc_api.c's
 * integer-only SF() field machinery: milli-units (x1000) for the two
 * dimensionless ratios (atten, gain), micrometers for the two lengths
 * (d0, zero_offset). Nominal atten=3 matches the board's actual A/B
 * attenuator (bench-confirmed, 2026-09 RC-filter investigation); gain
 * and d0 are un-bench-calibrated starting points, same "unconfirmed
 * against real hardware" caveat as DEFAULT_ENCODER_COUNTS_PER_DETENT
 * above. zero_offset starts at 0 (no bench zero calibration done yet). */
#define DEFAULT_DISP_ATTEN_MILLI            3000    /* atten = 3.000 */
#define DEFAULT_DISP_S1_GAIN_MILLI         10000    /* gain  = 10.000 */
#define DEFAULT_DISP_S1_D0_UM                100    /* d0    = 0.100 mm */
#define DEFAULT_DISP_S1_ZERO_OFFSET_UM         0
#define DEFAULT_DISP_S2_GAIN_MILLI         10000
#define DEFAULT_DISP_S2_D0_UM                100
#define DEFAULT_DISP_S2_ZERO_OFFSET_UM         0

/* --- Displacement zero calibration (180-degree reversal test, 2026-09-25) ---
 * Standard precision-level technique: place the instrument, average its
 * reading (step 1), physically rotate it 180 degrees IN PLACE, average
 * again (step 2). A real surface tilt phi contributes with opposite sign
 * to each step's reading (the instrument's own frame flips relative to
 * the surface), while the instrument's own zero error stays the same
 * (it's intrinsic to the sensor, not the surface) -- so the two steps'
 * average cancels phi and isolates the zero error:
 *   step1 = phi + zero_error, step2 = -phi + zero_error
 *   zero_error = (step1 + step2) / 2
 * Applied independently per sensor head (S1, S2 each have their own
 * disp_*_zero_offset_um -- see svc_displacement.c's
 * svc_displacement_zero_cal_*() and Services/svc_api.c's Commands
 * API2_RES_CMD_ZERO_CAL). Per-instrument by construction: this only ever
 * reads/writes THIS device's own g_device_settings and its own EEPROM --
 * nothing here is shared across physical units.
 *
 * Samples averaged per step. At the default DISPLACEMENT_BATCH_CYCLES
 * (32, ~81 batches/s), 128 samples =~ 1.6 s per step -- enough averaging
 * to ride out ordinary sensor noise (see the bulk-capture signal-quality
 * analysis) without making the user hold the instrument still for long. */
#define DISPLACEMENT_ZERO_CAL_SAMPLES        128U

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

/* --- Displacement phasor diagnostics (2026-09-25) ---
 * Exposes the demod's intermediate I/Q phasors (Services/svc_displacement.c's
 * BatchSums -- the batch-summed values feeding process_one_batch()'s
 * complex division, one step upstream of delta_mm/residual) for bench
 * diagnosis: a consistently-off phasor points at a calibration or wiring
 * problem in a way delta_mm alone can't distinguish from "device working,
 * instrument tilted".
 *
 * Real-time: API v2 Topic groups (0x5) resource API2_RES_TOPIC_PHASORS --
 * the latest completed batch's 8 floats, GET + SUBSCRIBE, same as any
 * other topic. ~81 updates/s available at the source (DISPLACEMENT_BATCH_CYCLES
 * below); a subscriber picks its own poll interval
 * (API2_MEASUREMENT_MIN_INTERVAL_MS floor, 50 ms) -- genuinely "real time"
 * is fine here specifically BECAUSE phasors are one bundled 32-byte
 * snapshot, not a per-sample stream like the raw ADC bulk capture below
 * (32 bytes @ 50 ms = 640 B/s, trivial next to UART's 115200 baud budget;
 * the raw ADC would be ~230x that).
 *
 * Longer time slots: API v2 Bulk (0x8) resource API2_RES_BULK_PHASORS.
 * Reuses the exact same accumulation/batching pipeline as normal
 * operation (on_sample() untouched) -- svc_displacement_update()'s
 * "batch complete" branch stores instead of demodulating while a capture
 * is armed. Storing every batch would only buy ~32x the raw-ADC capture's
 * ~295 ms window (batches complete DISPLACEMENT_BATCH_CYCLES=32x slower
 * than raw ADC samples) -- decimating further, storing only every
 * DISPLACEMENT_PHASOR_LOG_DECIMATIONth batch, trades that resolution for
 * duration instead: (2604.167/32)/2 =~ 40.7 Hz effective, matching the
 * old WP8 signal-analysis module's update rate (this constant was
 * re-derived 2026-09-25 when DISPLACEMENT_BATCH_CYCLES quadrupled 8->32
 * as part of the margin-hardening below it -- decimation dropped
 * 8->2 to land on the same ~40.7 Hz/~12.6 s target as before, not a
 * 4x-longer capture by accident). */
#define DISPLACEMENT_PHASOR_LOG_DECIMATION    2U

/* 512 entries x 34 B (8 floats + a u16 seq, packed) = 17408 B (~17 KB) --
 * at the ~40.7 Hz effective rate above, ~12.6 s of history. Picked to
 * leave a comfortable RAM margin alongside the raw-ADC capture buffer
 * (both exist, but the two captures are mutually exclusive at runtime so
 * only one is ever actively written at a time -- this is a static
 * allocation trade, not a runtime one). */
#define DISPLACEMENT_PHASOR_LOG_DEPTH          512U

/* Entries per bulk chunk packet. Payload is [page:1][entry:34]xN; the
 * whole API2 packet must fit API2_PACKET_MAX_SIZE (128): 4 (frame) + 1
 * (status) + 1 (page) + 34*N + 2 (crc) <= 128 -> N <= 3. */
#define DISPLACEMENT_PHASOR_LOG_CHUNK_ENTRIES  3U

/* Chunks pushed per svc_api_update() tick, upper bound -- same reasoning
 * as ADC_BULK_CHUNKS_PER_TICK above. */
#define DISPLACEMENT_PHASOR_LOG_CHUNKS_PER_TICK 4U

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
