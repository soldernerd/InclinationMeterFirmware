#ifndef SYSTEM_STATE_H
#define SYSTEM_STATE_H

#include <stdint.h>
#include <stdbool.h>

/* Field groups below are laid out contiguously per EEPROM page — see
 * config.h's EEPROM_*_SETTINGS_ADDR/VERSION and
 * Services/svc_storage.c's SettingsSection table, which slices this
 * struct into independently-versioned/CRC'd pages by exactly these
 * group boundaries (offsetof(first field) .. offsetof(last field) +
 * sizeof(last field)). Keep each group's fields adjacent — inserting an
 * unrelated field in the middle of a group would silently pull it into
 * that group's EEPROM page. No calibration constant lives only in
 * flash; these are always read from here, not a #define, past first
 * boot — see config.h's DEFAULT_* for the seed values. */
typedef struct {
    /* --- Scheduler/Timing page --- */
    uint16_t task_sensors_ms;
    uint16_t task_processing_ms;
    uint16_t task_display_ms;
    uint16_t task_ble_ms;
    uint16_t task_usb_ms;
    uint16_t task_battery_ms;
    uint16_t task_temperature_ms;
    uint16_t stream_interval_ms;
    int32_t  settling_threshold_umpm;
    uint32_t settling_timeout_ms;
    uint16_t filter_cutoff_hz_num;
    uint16_t filter_cutoff_hz_den;

    /* --- Battery page --- */
    uint16_t battery_critical_mv;   /* below this: BATTERY_CRITICAL */
    uint16_t battery_low_mv;        /* below this (and >= critical): BATTERY_LOW */
    uint16_t vbat_scale_num;
    uint16_t vbat_scale_den;

    /* --- TMP236 (on-board temp sensor) page --- */
    uint16_t tmp236_seg1_voffs_mv;
    uint16_t tmp236_seg1_num;
    uint16_t tmp236_seg1_den;
    uint16_t tmp236_seg_boundary_mv;
    uint16_t tmp236_seg2_voffs_mv;
    uint16_t tmp236_seg2_num;
    uint16_t tmp236_seg2_den;
    uint16_t tmp236_seg2_tinfl_cdeg;

    /* --- LM35 (external temp sensor) page --- */
    uint16_t lm35_scale_mv_per_c;

    /* --- Encoder page (WP3) ---
     * Raw quadrature transitions per mechanical detent — see
     * App/app_ui.c's consume_detents(). Unconfirmed against real
     * hardware, same as the calibration fields above. */
    uint16_t encoder_counts_per_detent;

    /* --- BLE page (WP5) ---
     * false -> Drivers_App/drv_rn4871.c runs the full RN4871
     * configuration sequence (factory reset, name, security, GATT
     * service/characteristics) on next boot instead of the abbreviated
     * $$$ -> A -> --- path; cleared from the SETTINGS screen's "Reset
     * BLE" action (App/app_ui.c) — deferred from WP4, added here now
     * that BLE actually exists to configure. */
    bool     ble_configured;

    /* Not persisted, never read outside this file's own boundary check.
     * A stable 1-byte anchor immediately after the last real settings
     * field, so Services/svc_storage.c's trailing-section _Static_assert
     * can require an EXACT offsetof match (like every other section
     * boundary) instead of tolerating a few bytes of slop to account for
     * the struct's own trailing alignment padding — that slop couldn't
     * distinguish real padding from a small field silently added after
     * ble_configured without a matching EEPROM section. Keep this the
     * last member; add new settings fields above it, inside their group. */
    uint8_t  _settings_end_marker;
} DeviceSettings;

typedef struct {
    int16_t  scl3300_x_cdeg;
    int16_t  scl3300_y_cdeg;
    int16_t  scl3300_z_cdeg;
    int32_t  pcap04_1_af;
    int32_t  pcap04_2_af;
    int16_t  temperature_cdeg;

    int32_t  tilt_pcap04_umpm;
    int32_t  tilt_scl3300_x_umpm;
    int32_t  tilt_scl3300_y_umpm;
    int32_t  tilt_scl3300_z_umpm;

    uint8_t  battery_soc_pct;
    bool     battery_charging;
    bool     battery_critical;

    bool     ble_connected;
    bool     usb_connected;
    bool     woke_from_standby;   /* true if this boot resumed from Standby
                                    * mode rather than a power-on/other reset —
                                    * see HAL_App/hal_power.h */

    bool     sensor_scl3300_ok;
    bool     sensor_pcap04_1_ok;
    bool     sensor_pcap04_2_ok;

    /* BME280 environmental sensor (WP9, Drivers_App/drv_bme280.c),
     * shares I2C1 with the EEPROM. bme280_temp_cdeg is a THIRD,
     * independent temperature reading -- not the same sensor as
     * temperature_cdeg above (despite this field's name/position, that's
     * NOT a stale pre-REV-B SCL3300/PCAP04-era reading -- it was
     * repurposed and is populated by REV B's own on-board TMP236, see
     * App/app_scheduler.c's task_temperature() and
     * Drivers_App/drv_tmp236.c; this comment previously described it as
     * legacy, which was wrong and got fixed here 2026-08-19, WP11) or
     * lm35_temp_cdeg below (TEMP_SENSE_EXT). Updated once per second by
     * App/app_scheduler.c's task_bme280() (mirrors task_temperature()'s
     * TMP236 pattern -- no Services-layer wrapper needed since
     * drv_bme280.c already does all the compensation math itself);
     * bme280_ok stays false until the first successful conversion. */
    int16_t  bme280_temp_cdeg;         /* 0.01 degC / LSB */
    uint32_t bme280_pressure_pa;       /* Pa */
    uint16_t bme280_humidity_centipct; /* 0.01 %RH / LSB */
    bool     bme280_ok;

    /* LM35 external temperature sensor (WP11, Drivers_App/drv_lm35.c) --
     * TEMP_SENSE_EXT (PB11/ADC1_IN15), previously sampled by hal_adc.c
     * but never converted/published (raw ADC channel existed, no driver
     * layer). Updated by the same App/app_scheduler.c task_temperature()
     * call that updates temperature_cdeg above -- both channels come
     * from the same shared ADC scan. No validity/"ok" companion field,
     * matching temperature_cdeg's own precedent: an unpopulated LM35 and
     * a populated-but-stale reading are indistinguishable from a bare
     * ADC voltage with no I2C-ACK-style presence signal (unlike
     * bme280_ok/sensor_pcap04_*_ok above, which come from a real
     * comms-level ok/fail check). */
    int16_t  lm35_temp_cdeg;           /* 0.01 degC / LSB */

    /* Displacement sensors S1/S2 (WP10), published by
     * Services/svc_displacement.c -- most-recent-cycle snapshot only,
     * for a quick "current value" read (future UI/API use). The full
     * per-cycle stream (every ~384 us carrier cycle, not just the
     * latest one) is NOT here -- it lives in svc_displacement.c's own
     * ring buffer, drained via its own getter, since a raw high-rate
     * stream doesn't fit this struct's "current state" shape. residual
     * is Im(x) from the complex division (see svc_displacement.c) --
     * should sit near 0 if the physical model holds; a diagnostic, not
     * itself part of the displacement result. */
    float    disp1_delta_mm;
    float    disp1_residual;
    float    disp2_delta_mm;
    float    disp2_residual;
    bool     disp_ok;   /* true once at least one cycle has been fully computed */

    bool     calibration_valid;

    /* Local UI input (WP3), published by Services/svc_input.c. Raw
     * quadrature transition counts, not mechanical-detent counts — see
     * Drivers_App/drv_encoder.h. App/app_ui.c is the UI layer that
     * translates these (and the press events) into navigation/edit
     * actions, deciding on its own how many raw counts make one
     * mechanical "click." */
    int32_t  encoder1_count;
    int32_t  encoder2_count;
    bool     encoder1_sw_pressed;        /* current level */
    bool     encoder2_sw_pressed;
    bool     encoder1_sw_press_event;    /* latched on press edge; the
                                           * consumer (app_ui.c) clears it
                                           * after acting on it — buttons
                                           * aren't EXTI-capable on this
                                           * pinout (see pin_config.h), so
                                           * svc_input.c polls and can't
                                           * hand the consumer a real
                                           * one-shot interrupt event */
    bool     encoder2_sw_press_event;

    /* True while an EEPROM write (settings OR calibration) has failed
     * and hasn't been superseded by a successful one yet. Set from:
     * (1) App/app_ui.c's commit_edit(), synchronously, if
     * svc_storage_save_settings() can't even be queued; (2)
     * Services/svc_api.c's SET_ZERO/SET_CALIBRATION/SET_SETTINGS
     * handlers, the same way, for a host-triggered save; (3)
     * Services/svc_storage.c's svc_storage_update(), asynchronously,
     * possibly many ticks later, if any in-flight write (settings or
     * calibration) fails partway through after retries are exhausted —
     * a synchronous DRV_OK only means the write was queued, not that it
     * completed, so this is the only signal a "successful" queue didn't
     * actually finish; (4) svc_storage_init()'s boot-time per-page
     * reseed-to-defaults write can also fail and sets it. Only
     * svc_storage_update() clears it, at the point an in-flight write
     * genuinely completes — not commit_edit()'s/svc_api.c's optimistic
     * synchronous-queue-success clear, which can't see a later failure.
     * svc_api.c's handlers NACK without setting this flag when
     * svc_storage_is_busy() reports transient contention (another save
     * already in flight) — that's not a failure, just "try again".
     * No DBG_PRINT infra exists in this codebase yet (WP1.5 was never
     * wired up), so this is the escalation-to-system-state half of
     * CLAUDE.md's "No Silent Failures" rule; App/app_display.c's
     * SETTINGS screen renders it. */
    bool     settings_save_failed;

    /* Counts USB HID IN reports svc_usb.c's send_via_usb() asked
     * hal_usb_send() to transmit but that hal_usb_send() reported
     * as failed (not connected, or USBD_CUSTOM_HID_SendReport returned
     * USBD_BUSY — a previous IN report still in flight). There's no
     * retry queue: a dropped notification is just gone. No DBG_PRINT
     * infra exists yet, so this counter is the CLAUDE.md 7.6 escalation
     * for that failure — makes it observable (e.g. via a future debug
     * command or GET_STATUS extension) instead of silent. Saturates
     * rather than wraps; never cleared once the count is nonzero. */
    uint16_t usb_tx_dropped_count;

    /* Same as usb_tx_dropped_count above, but for Services/svc_ble.c's
     * send_via_ble() / drv_rn4871_send_notification() path — a failed
     * BLE notification (not connected, or the UART TX DMA already busy)
     * is equally silent otherwise. */
    uint16_t ble_tx_dropped_count;

    /* Same again, but for Services/svc_uart.c's send_via_uart() path
     * (WP5.1) — the wired debug-UART transport's TX DMA already busy. */
    uint16_t uart_tx_dropped_count;

    /* Services/svc_api.c (WP11 API v2) — a received frame whose declared
     * LEN doesn't fit the bytes actually delivered, or whose CRC-valid
     * response payload would exceed MAX_PAYLOAD, is dropped with no
     * response (CLAUDE.md 7.6 escalation). Deliberately a counter, not a
     * DBG_PRINT call: API_TRANSPORT_UART and the still-unimplemented
     * DBG_PRINT() debug-log channel share the same physical wire
     * (USART3, see docs/wp2-5_rebase_status.md's WP5.1 section) —
     * emitting debug text from svc_api.c, which is transport-agnostic
     * and runs this same code regardless of which transport triggered
     * it, would corrupt the UART transport's own live protocol stream
     * exactly when a UART-connected host is most likely to be actively
     * testing. Saturates rather than wraps; never cleared once nonzero,
     * matching every other drop counter here. */
    uint16_t api_rx_malformed_count;
} SystemState;

typedef struct {
    /* Scale constants — set at manufacturing */
    int32_t  pcap04_scale_af_per_umpm;
    int32_t  scl3300_scale_cdeg_per_umpm;

    /* Zero offsets — set by operator */
    int32_t  pcap04_zero_af;
    int16_t  scl3300_x_zero_cdeg;
    int16_t  scl3300_y_zero_cdeg;
    int16_t  scl3300_z_zero_cdeg;

    uint32_t calibration_timestamp;
    bool     scale_valid;
    bool     zero_valid;
} CalibrationData;

/* Displacement sensor S1/S2 calibration (WP10) -- one instance per
 * sensor (g_disp_s1_cal/g_disp_s2_cal below), each its own small
 * standalone EEPROM page via Services/svc_storage.c's generic blob
 * save/load (svc_storage_save_blob()/load_blob()), same pattern
 * CalibrationData above already established (a dedicated struct + page,
 * NOT threaded through DeviceSettings' SettingsSection/offsetof
 * mechanism). This is deliberate, not an oversight: DeviceSettings is
 * sent whole in one 60-byte USB HID report (Services/svc_api.c's
 * MAX_PAYLOAD, enforced by a _Static_assert there) and was already near
 * that ceiling -- these three float fields alone would have blown it.
 * See Services/svc_displacement.c for how gain/d0_mm/zero_offset_mm
 * combine with DisplacementSharedCal's atten below to compute
 * x/delta per sensor. First float fields in this codebase's persisted
 * calibration data -- every existing CalibrationData field above is a
 * scaled integer, but this is the Math/Services layer, where floats are
 * explicitly permitted (CLAUDE.md's "no floating point" rule is scoped
 * to HAL/driver layers), and a gain ratio / sub-mm gap value is a
 * natural fit for one. */
typedef struct {
    float gain;             /* S-channel amplifier gain, nominal 10 */
    float d0_mm;             /* neutral-position air gap, mm, nominal 0.1 */
    float zero_offset_mm;    /* displacement zero calibration, mm */
} DisplacementSensorCal;

/* atten is instrument-wide, not per-sensor: both S1 and S2 heads are
 * driven from the same antiphase excitation pair through the same
 * attenuator ahead of the ADC, so there is exactly one value, unlike
 * DisplacementSensorCal's gain above (each head's own amplifier, can
 * differ) -- its own tiny struct/page rather than folded into either
 * sensor's, matching this file's existing convention of separating
 * anything that can be recalibrated/versioned independently. */
typedef struct {
    float atten;
} DisplacementSharedCal;

extern SystemState    g_system_state;
extern DeviceSettings g_device_settings;
extern CalibrationData g_calibration;
extern DisplacementSensorCal g_disp_s1_cal;
extern DisplacementSensorCal g_disp_s2_cal;
extern DisplacementSharedCal g_disp_shared_cal;

#endif /* SYSTEM_STATE_H */
