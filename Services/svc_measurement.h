#ifndef SVC_MEASUREMENT_H
#define SVC_MEASUREMENT_H

#include <stdint.h>
#include <stdbool.h>

typedef enum {
    MEAS_STATE_IDLE = 0,
    MEAS_STATE_SETTLING,
    MEAS_STATE_CAPTURING,
    MEAS_STATE_COMPLETE,
} MeasurementState;

typedef struct {
    int32_t  tilt_pcap04_umpm;
    int32_t  tilt_scl3300_x_umpm;
    int32_t  tilt_scl3300_y_umpm;
    int16_t  temperature_cdeg;
    uint8_t  battery_soc_pct;
    uint8_t  status_flags;          /* bit0 SCL3300, bit1 PCAP04_1, bit2 PCAP04_2 */
    uint32_t timestamp_ms;
    uint16_t sample_count;
} MeasurementPacket;

void                      svc_measurement_init(void);
void                      svc_measurement_update(void);
void                      svc_measurement_trigger(void);
void                      svc_measurement_cancel(void);
MeasurementState          svc_measurement_get_state(void);
const MeasurementPacket  *svc_measurement_get_packet(void);
void                      svc_measurement_acknowledge(void);

/* Progress in percent (0..100) — used for the display overlay and
 * SINGLE_PROGRESS notifications. */
uint8_t                   svc_measurement_get_progress_pct(void);

#endif /* SVC_MEASUREMENT_H */
