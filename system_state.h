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
    uint16_t battery_cutoff_mv;
    uint8_t  battery_low_pct;
    uint8_t  battery_critical_pct;
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

    bool     sensor_scl3300_ok;
    bool     sensor_pcap04_1_ok;
    bool     sensor_pcap04_2_ok;

    bool     calibration_valid;
} SystemState;

extern SystemState    g_system_state;
extern DeviceSettings g_device_settings;

#endif /* SYSTEM_STATE_H */
