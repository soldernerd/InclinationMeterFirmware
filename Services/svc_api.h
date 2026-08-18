#ifndef SVC_API_H
#define SVC_API_H

#include <stdint.h>
#include <stdbool.h>

typedef enum {
    API_TRANSPORT_USB  = 0,
    API_TRANSPORT_BLE  = 1,
    API_TRANSPORT_UART = 2,
    API_TRANSPORT_COUNT,
} ApiTransport;

typedef enum {
    API_MODE_IDLE = 0,
    API_MODE_SINGLE,
    API_MODE_STREAM,
    API_MODE_RAW_STREAM,
    API_MODE_DISP_STREAM,   /* WP10 dual-sensor displacement cycle stream --
                              * see API_CMD_START_DISP_STREAM. Named
                              * specifically (not a generic "raw stream 2")
                              * because more per-subsystem raw streams are
                              * expected later (e.g. single-channel raw ADC),
                              * each wanting its own distinctly-named command
                              * pair rather than one overloaded mode. */
} ApiMode;

typedef void (*ApiSendFn)(const uint8_t *data, uint16_t len);

void svc_api_init(void);
void svc_api_update(void);

void svc_api_register_transport(ApiTransport t, ApiSendFn send_fn);
void svc_api_connected(ApiTransport t);
void svc_api_disconnected(ApiTransport t);

void svc_api_receive(ApiTransport t, const uint8_t *data, uint16_t len);

/* On-the-wire packet framing, shared with any transport whose HAL layer
 * delivers raw bytes rather than whole frames (BLE transparent UART, the
 * wired debug UART) and so needs to reassemble packets itself — USB HID
 * delivers whole reports already framed by the endpoint, so svc_usb.c
 * doesn't use this. */
#define API_PACKET_HDR_BYTES   2U
#define API_PACKET_CRC_BYTES   2U
#define API_PACKET_MAX_SIZE    64U   /* must equal USB_HID_REPORT_SIZE — see svc_api.c's _Static_assert */

typedef struct {
    uint8_t  buf[API_PACKET_MAX_SIZE];
    uint16_t pos;
    uint32_t started_ms;
} ApiByteReassembler;

/* Feeds one received byte in. Once a complete, CRC-framed packet is
 * assembled it's dispatched via svc_api_receive(t, ...) and the
 * reassembler resets itself for the next packet — callers just feed
 * bytes as they arrive and separately call
 * svc_api_reassembler_check_timeout() to abandon a stalled partial
 * packet. */
void svc_api_reassembler_feed_byte(ApiTransport t, ApiByteReassembler *r, uint8_t b);
void svc_api_reassembler_check_timeout(ApiByteReassembler *r, uint32_t timeout_ms);

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
#define API_CMD_START_DISP_STREAM   0x0EU   /* USB only -- see svc_api.c's dispatch() */
#define API_CMD_STOP_DISP_STREAM    0x0FU

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
#define API_NOTIFY_DISP_STREAM_DATA 0xF5U

/* Drains up to a few pending Services/svc_displacement.c cycles into one
 * USB HID report per call, for any transport currently in
 * API_MODE_DISP_STREAM. Unlike svc_api_update()'s other stream modes,
 * this is NOT gated by an elapsed-time interval -- the ~2.6 kHz
 * production rate needs draining every scheduler tick to keep the
 * output ring from falling behind, so this is polled by its own
 * dedicated every-tick scheduler task rather than svc_api_update()'s
 * existing task_sensors_ms-period task. Call every tick. */
void svc_api_disp_stream_update(void);

#endif /* SVC_API_H */
