#ifndef SVC_API_H
#define SVC_API_H

#include <stdint.h>
#include <stdbool.h>

typedef enum {
    API_TRANSPORT_USB = 0,
    API_TRANSPORT_BLE = 1,
    API_TRANSPORT_COUNT,
} ApiTransport;

typedef enum {
    API_MODE_IDLE = 0,
    API_MODE_SINGLE,
    API_MODE_STREAM,
    API_MODE_RAW_STREAM,
} ApiMode;

typedef void (*ApiSendFn)(const uint8_t *data, uint16_t len);

void svc_api_init(void);
void svc_api_update(void);

void svc_api_register_transport(ApiTransport t, ApiSendFn send_fn);
void svc_api_connected(ApiTransport t);
void svc_api_disconnected(ApiTransport t);

void svc_api_receive(ApiTransport t, const uint8_t *data, uint16_t len);

void svc_api_notify_single_ready(void);

ApiMode svc_api_get_mode(ApiTransport t);

/* Command + response opcodes */
#define API_CMD_GET_STATUS          0x01U
#define API_CMD_REQUEST_SINGLE      0x02U
#define API_CMD_CANCEL_SINGLE       0x03U
#define API_CMD_START_STREAM        0x04U
#define API_CMD_STOP_STREAM         0x05U
#define API_CMD_START_RAW_STREAM    0x06U
#define API_CMD_STOP_RAW_STREAM     0x07U
#define API_CMD_SET_ZERO            0x08U
#define API_CMD_GET_CALIBRATION     0x09U
#define API_CMD_SET_CALIBRATION     0x0AU
#define API_CMD_GET_SETTINGS        0x0BU
#define API_CMD_SET_SETTINGS        0x0CU
#define API_CMD_GET_IDENTITY        0x0DU

#define API_RSP_GET_STATUS          0x81U
#define API_RSP_REQUEST_SINGLE      0x82U
#define API_RSP_GET_CALIBRATION     0x89U
#define API_RSP_GET_SETTINGS        0x8BU
#define API_RSP_GET_IDENTITY        0x8DU
#define API_RSP_ACK                 0xA0U
#define API_RSP_NACK                0xA1U

#define API_NOTIFY_SINGLE_READY     0xF0U
#define API_NOTIFY_SINGLE_PROGRESS  0xF1U
#define API_NOTIFY_STREAM_DATA      0xF2U
#define API_NOTIFY_RAW_STREAM_DATA  0xF3U
#define API_NOTIFY_STATUS_CHANGED   0xF4U

#endif /* SVC_API_H */
