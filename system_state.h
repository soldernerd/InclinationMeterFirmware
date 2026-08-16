#ifndef SYSTEM_STATE_H
#define SYSTEM_STATE_H

#include <stdint.h>
#include <stdbool.h>

typedef struct {
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
    uint16_t battery_critical_mv;   /* below this: BATTERY_CRITICAL */
    uint16_t battery_low_mv;        /* below this (and >= critical): BATTERY_LOW */

    /* ADC/sensor scaling calibration — see config.h's DEFAULT_* for the
     * seed values and what each feeds. No calibration constant lives
     * only in flash; these are always read from here, not a #define,
     * past first boot. */
    uint16_t vbat_scale_num;
    uint16_t vbat_scale_den;
    uint16_t tmp236_seg1_voffs_mv;
    uint16_t tmp236_seg1_num;
    uint16_t tmp236_seg1_den;
    uint16_t tmp236_seg_boundary_mv;
    uint16_t tmp236_seg2_voffs_mv;
    uint16_t tmp236_seg2_num;
    uint16_t tmp236_seg2_den;
    uint16_t tmp236_seg2_tinfl_cdeg;
    uint16_t lm35_scale_mv_per_c;
    uint16_t checksum;
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
