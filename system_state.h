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

extern SystemState    g_system_state;
extern DeviceSettings g_device_settings;
extern CalibrationData g_calibration;

#endif /* SYSTEM_STATE_H */
