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
    uint16_t task_display_ms;
    uint16_t task_ble_ms;
    uint16_t task_usb_ms;
    uint16_t task_battery_ms;
    uint16_t task_temperature_ms;

    /* --- Battery page --- */
    uint16_t battery_critical_mv;     /* below this: BATTERY_CRITICAL (power-off) */
    uint16_t battery_low_mv;          /* below this (and >= critical): BATTERY_LOW (warning) */
    uint16_t battery_charge_start_mv; /* below this, with USB present: enable charging */
    uint16_t vbat_scale_num;         /* ADC divider scale — part of the battery page */
    uint16_t vbat_scale_den;
    uint16_t auto_poweroff_s;        /* WP6: idle seconds before auto power-off into
                                      * Standby; 0 = disabled. Was battery_page_reserved
                                      * (a 4-byte-alignment pad); giving it a purpose
                                      * keeps DeviceSettings the same size. Bumped
                                      * EEPROM_BATTERY_SETTINGS_VERSION to 0x0003 so a
                                      * stored v2 battery page reseeds from DEFAULT_*. */
    int32_t  vbat_offset_mv;         /* int32 (not int16) to keep DeviceSettings a
                                      * multiple of 4 — see svc_storage.c's section
                                      * static_asserts. Additive Vbat correction, applied
                                      * after the
                                      * vbat_scale_num/den ratio in svc_battery.c's
                                      * adc_to_vbat_mv(). The battery-sense path was
                                      * bench-measured (Keysight supply, 3.5-4.3 V) to
                                      * have a ~constant -80 mV error, not a gain error
                                      * — this term fixes that. Range +/-500 mV
                                      * (API Settings 0x1C). Bumped
                                      * EEPROM_BATTERY_SETTINGS_VERSION to 0x0004. */

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
     * hardware. */
    uint16_t encoder_counts_per_detent;

    /* --- Displacement calibration page (WP10) ---
     * Differential-capacitor sensor calibration — see
     * Services/svc_displacement.c for the demod math these feed and
     * config.h's DEFAULT_DISP_* for the nominal values. Scaled integers
     * (milli-units — x1000 — for the two dimensionless ratios, plain
     * micrometers for the two lengths), not raw floats: Services/svc_api.c's
     * SF() field machinery is integer-only, matching every other
     * calibration constant in this struct. svc_displacement.c converts
     * to float once per carrier cycle, where CLAUDE.md permits floating
     * point (Math/Services layer, not HAL/driver). Exposed over the API
     * under Calibrations (category 0x2), not Settings (0x3) — the first
     * resources to use that category. */
    int32_t disp_atten_milli;        /* shared A/B attenuator, x1000 (nominal 3.000) */
    int32_t disp_s1_gain_milli;      /* S1 amplifier gain, x1000 (nominal 10.000) */
    /* d0 REPLACED 2026-09-27 by a theoretical-baseline + a calibration
     * factor (config.h's "Displacement sensitivity: theoretical baseline"
     * comment has the full derivation). d0_theoretical is the
     * Wyler-handbook-derived micrometers-per-x-unit constant (assuming the
     * nominal 20uV RMS = 1um/m spec exactly).
     *
     * sensitivity_uv_per_um_milli (2026-09-29, replacing the original
     * cal_mult_milli -- a bare dimensionless ratio, which is exactly the
     * kind of indirect representation that caused real confusion this
     * session: "why is the correction factor 2.7x, I expected ~1x?" has an
     * immediate answer when the field IS the physical quantity being
     * calibrated, not a multiplier on an assumption buried in firmware).
     * This is the sensor's REAL, measured electrical sensitivity: how many
     * microvolts (RMS, at the ADC pin, same reference point as the
     * DIAGNOSTICS screen / signal-diag RMS values) this specific sensor
     * actually produces per 0.001mm/m (1um/m) of real tilt -- calibrated
     * directly against a known applied tilt, nothing else. Compares
     * directly against the fixed nominal spec
     * (DISPLACEMENT_WYLER_UV_RMS_PER_UM_PER_M, config.h, =20) with no unit
     * conversion needed: this sensor's real sensitivity IS this many uV,
     * period, vs. the datasheet's 20uV claim.
     * Services/svc_displacement.c's load_sensor_cal() converts this to the
     * old internal cal_mult concept (cal_mult = nominal/sensitivity) before
     * computing effective_d0 -- that's purely an internal implementation
     * detail now, not something this field's meaning depends on. */
    int32_t disp_s1_d0_theoretical_um;
    int32_t disp_s1_sensitivity_uv_per_um_milli;
    /* S1 displacement zero calibration, micrometers, signed -- stored in the
     * cal_mult-INDEPENDENT theoretical domain (i.e. what this sensor's
     * electrical zero error would read with cal_mult=1.0), NOT the final
     * output-mm domain the name might suggest -- see
     * Services/svc_displacement.c's load_sensor_cal() comment (2026-09-29)
     * for why: the sensor's zero error lives in x_re itself, so it has to
     * scale with cal_mult like any other x-domain quantity, not sit fixed
     * in mm. Written by svc_api.c's zero_cal_apply_if_ready(), which
     * divides by cal_mult before persisting here. */
    int32_t disp_s1_zero_offset_um;
    int32_t disp_s2_gain_milli;      /* S2 amplifier gain, x1000 */
    int32_t disp_s2_d0_theoretical_um;
    int32_t disp_s2_sensitivity_uv_per_um_milli;  /* S2 -- same meaning as disp_s1_sensitivity_uv_per_um_milli above */
    int32_t disp_s2_zero_offset_um;  /* S2 zero calibration -- same theoretical domain as disp_s1_zero_offset_um above */

    /* Sign convention is arbitrary at the sensor level -- these flip the
     * FINAL reported reading (after gain/zero-cal, at the getter layer in
     * svc_displacement.c) so "front up" can be made to read positive per
     * instrument, without touching the zero-cal/gain math itself. 0 =
     * normal, 1 = inverted. Deliberately NOT folded into the sensitivity's
     * sign -- doing so would silently invalidate the sensor's existing
     * zero_offset the same way changing cal_mult's magnitude does (see
     * 2026-09-29 granite-plate session). */
    uint8_t disp_s1_invert;
    uint8_t disp_s2_invert;
    /* Phase calibration (2026-10-06, docs/signal_processing.tex Sec. 9.1,
     * approach 2): phase delta of each sensor's signal relative to the
     * run-time reference D = A - B, in centidegrees (firmware angle
     * convention: positive = the sensor signal is DELAYED relative to the
     * ideal in-phase position, negative = earlier). This is the phase of
     * the complex gain ratio k = |k| e^{j delta}; the sensor phasor is
     * rotated by -delta before the division by D, so the reading is the
     * exact in-phase component. No constants for A and B are needed (D is
     * measured in every batch), and the absolute sample-grid phase drops
     * out. */
    int16_t disp_s1_phase_cdeg;
    int16_t disp_s2_phase_cdeg;
    /* Explicit, not compiler-inserted: the struct's overall alignment (4,
     * from its int32_t members) would otherwise leave 2 silent trailing
     * pad bytes after disp_s2_invert, which svc_storage.c's SECTION_SPAN/
     * _Static_assert machinery can't see (it measures declared fields,
     * not compiler padding) -- making it deliberate here keeps that
     * "section must end exactly at the struct's end" assert meaningful.
     * Never read/written; exists purely so this struct's layout has no
     * bytes the EEPROM section machinery doesn't know about. */
    uint8_t disp_reserved_pad[2];
} DeviceSettings;

typedef struct {
    int16_t  temperature_cdeg;      /* TMP236 on-board (drv_tmp236.c) */

    /* LM35 external temperature sensor (WP11, Drivers_App/drv_lm35.c),
     * TEMP_SENSE_EXT / PB11 / ADC1_IN15. Independent of temperature_cdeg
     * (TMP236) and the BME280. temp_ext_ok is false until a plausible
     * reading exists — a disconnected/faulted LM35 reads out of range and
     * drv_lm35_get_result() rejects it. */
    int16_t  temp_ext_cdeg;        /* 0.01 degC / LSB */
    bool     temp_ext_ok;

    uint8_t  battery_soc_pct;
    bool     battery_charging;
    bool     battery_low;         /* Vbat below battery_low_mv (implies the warning
                                    * screen); also true whenever battery_critical is */
    bool     battery_critical;

    bool     ble_connected;
    bool     usb_connected;       /* "USB power present" (VBUS_SENSE high) — NOT
                                    * "enumerated to a host". Sole writer:
                                    * Services/svc_battery.c. For host/enumeration
                                    * state, call hal_usb_is_connected(). */
    bool     woke_from_standby;   /* true if this boot resumed from Standby
                                    * mode rather than a power-on/other reset —
                                    * see HAL_App/hal_power.h */

    bool     adc_ok;             /* ADC calibrated OK at boot; if false,
                                  * battery/temperature readings are absent
                                  * (not a boot-halting condition) */

    bool     dac_ok;             /* AD9833 DAC (WP7) init succeeded at boot
                                  * — all 5 SPI3 setup writes returned OK.
                                  * If false the waveform output is absent;
                                  * not boot-halting (write-only device, no
                                  * readback to confirm anyway). */

    bool     ads_ok;             /* ADS131M04 external ADC (WP8) init +
                                  * svc_displacement wiring (WP10)
                                  * succeeded at boot. If false the
                                  * displacement readout is absent; not
                                  * boot-halting. */

    /* Differential-capacitor displacement (WP10) latest-value state
     * deliberately does NOT live here -- see Services/svc_displacement.h's
     * top comment: writing those float fields into g_system_state was
     * bench-reproducible as an instant hang (root cause unresolved).
     * svc_displacement_get_delta1_mm()/_get_residual1()/_get_delta2_mm()/
     * _get_residual2()/_get_ok() are the equivalent of ads_ok above, for
     * Sensor 1 (CH3) / Sensor 2 (CH0). */

    bool     display_ok;         /* False if the Sharp LCD's SPI2 peripheral
                                  * wedged and a flush had to be abandoned —
                                  * see drv_sharp_lcd_update(). Self-clears
                                  * back to true once a flush completes
                                  * cleanly again. Not boot-halting; the
                                  * on-screen content is simply stale/absent
                                  * while false. */

    /* Boot EEPROM write->read-back self-test (svc_storage_init()):
     *   255 = not run   0 = pass
     *   1 = write start rejected      2 = write DMA NAK'd (data never reached chip)
     *   3 = write-cycle ACK-poll timeout
     *   4 = read-back DMA failed      5 = read-back mismatch (pass A)
     *   6 = read-back mismatch (pass B, inverted pattern) */
    uint8_t  eeprom_selftest;

    /* BME280 environmental sensor (WP9, Drivers_App/drv_bme280.c),
     * shares I2C1 with the EEPROM. bme280_temp_cdeg is a THIRD,
     * independent temperature reading -- not the same sensor as
     * temperature_cdeg above (the TMP236 on-board sensor, drv_tmp236.c /
     * svc_battery.c) or temp_ext_cdeg (the LM35 external channel).
     * Updated once per second by App/app_scheduler.c's
     * task_bme280() (mirrors task_temperature()'s TMP236 pattern -- no
     * Services-layer wrapper needed since drv_bme280.c already does all
     * the compensation math itself); bme280_ok stays false until the
     * first successful conversion. */
    int16_t  bme280_temp_cdeg;         /* 0.01 degC / LSB */
    uint32_t bme280_pressure_pa;       /* Pa */
    uint16_t bme280_humidity_centipct; /* 0.01 %RH / LSB */
    bool     bme280_ok;

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

    /* True while an EEPROM settings write has failed and hasn't been
     * superseded by a successful one yet. Set from:
     * (1) App/app_ui.c's commit_edit(), synchronously, if
     * svc_storage_save_settings() can't even be queued; (2)
     * Services/svc_api.c's SET_SETTINGS handler, the same way, for a
     * host-triggered save; (3)
     * Services/svc_storage.c's svc_storage_update(), asynchronously,
     * possibly many ticks later, if any in-flight write fails partway
     * through after retries are exhausted —
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

    /* Counts API frames svc_usb.c's send_via_usb() could not enqueue in
     * the per-transport TX ring (Services/svc_txframe.c) because it was
     * full past the point its admission rule allows for that frame.
     * hal_usb_send() returning USBD_BUSY is NOT counted here — that frame
     * stays queued and is retried on the next pump. There's no retry
     * queue for a ring-full drop: the frame is gone. This counter is the
     * CLAUDE.md 7.6 escalation for that — observable via a GET on the API
     * or the debug-log WARN emitted on the first drop of each full
     * episode. Saturates rather than wraps; never cleared once nonzero. */
    uint16_t usb_tx_dropped_count;

    /* Same as usb_tx_dropped_count, for the BLE transport: svc_ble.c's
     * send_via_ble() could not enqueue a frame in the per-transport TX
     * ring (Services/svc_txframe.c) — the ring was full past the point
     * its admission rule allows for this frame. No retry queue — the
     * dropped frame is gone. Saturates rather than wraps. */
    uint16_t ble_tx_dropped_count;

    /* Same as ble_tx_dropped_count, for the wired UART transport
     * (Services/svc_uart.c, USART3 debug header). Saturates. */
    uint16_t uart_tx_dropped_count;

    /* API v2: incremented by svc_api.c whenever a received frame can't be
     * dispatched or answered — too short to hold a header+CRC, a declared
     * LEN that doesn't match what arrived, or an oversized response a
     * resource handler tried to build. CLAUDE.md 7.6 escalation for the
     * cases where echoing a status back isn't possible or safe. */
    uint16_t api_rx_malformed_count;
} SystemState;

extern SystemState    g_system_state;
extern DeviceSettings g_device_settings;

#endif /* SYSTEM_STATE_H */
