#ifndef SVC_API_H
#define SVC_API_H

#include <stdint.h>
#include <stdbool.h>

/* Device API v2 -- see docs/api-v2-spec.md for the design rationale and
 * docs/api-reference.md for the host-facing contract. This header is the
 * implementation-side counterpart: opcode/status enums, packet framing
 * constants, and the transport-facing entry points. Transports
 * (Services/svc_usb.c/svc_ble.c/svc_uart.c) never need anything beyond
 * what's declared here -- all protocol logic lives in svc_api.c. */

typedef enum {
    API_TRANSPORT_USB  = 0,
    API_TRANSPORT_BLE  = 1,
    API_TRANSPORT_UART = 2,
    API_TRANSPORT_COUNT,
} ApiTransport;

typedef void (*ApiSendFn)(const uint8_t *data, uint16_t len);

void svc_api_init(void);
void svc_api_update(void);

void svc_api_register_transport(ApiTransport t, ApiSendFn send_fn);
void svc_api_connected(ApiTransport t);
void svc_api_disconnected(ApiTransport t);

void svc_api_receive(ApiTransport t, const uint8_t *data, uint16_t len);

/* ---------------- packet framing (docs/api-v2-spec.md §2) ----------------
 * [OPCODE 2B LE][LEN 2B LE][PAYLOAD 0..LEN][CRC16 2B LE], no padding.
 * Total on the wire is always 6 + LEN. */
#define API2_PACKET_HDR_BYTES   4U   /* OPCODE(2) + LEN(2) */
#define API2_PACKET_CRC_BYTES   2U

/* Reassembly buffer ceiling for byte-stream transports (BLE/UART) and the
 * largest single packet USB will build/parse without needing multi-report
 * chaining (spec §2.2). NOT yet the general "arbitrarily long LEN" the
 * spec allows in principle -- every stage-1 resource fits with headroom.
 * Multi-report/multi-frame chaining for payloads exceeding this is
 * deferred to whichever category first needs it (Debug messages/Raw
 * data/Bulk are the likely candidates) -- flagged in the stage-1 check-in,
 * not silently decided. Revisit this constant when that's built. */
#define API2_PACKET_MAX_SIZE    128U

typedef struct {
    uint8_t  buf[API2_PACKET_MAX_SIZE];
    uint16_t pos;
    uint32_t started_ms;
} ApiByteReassembler;

/* Feeds one received byte in. Once a complete, CRC-framed packet is
 * assembled it's dispatched via svc_api_receive(t, ...) and the
 * reassembler resets itself for the next packet -- callers just feed
 * bytes as they arrive and separately call
 * svc_api_reassembler_check_timeout() to abandon a stalled partial
 * packet. Unchanged in shape from v1, just keyed off the new 4-byte
 * header/2-byte LEN instead of v1's 2-byte header/1-byte LEN. */
void svc_api_reassembler_feed_byte(ApiTransport t, ApiByteReassembler *r, uint8_t b);
void svc_api_reassembler_check_timeout(ApiByteReassembler *r, uint32_t timeout_ms);

/* ---------------- opcode structure (docs/api-v2-spec.md §3) ----------------
 * 16 bits: [VERB:4][CATEGORY:4][RESOURCE INDEX:8], VERB in the top nibble.
 * (Confirmed with the user 2026-08-19: the spec's §3 diagram and its
 * §3.4/§5 "top 4 bits" wording for category directly disagreed on which
 * field is most significant. VERB-on-top is authoritative; the spec's
 * "top 4 bits" phrasing for category is the stale/wrong part and should
 * be read as "the category field", not literally the most-significant
 * bits.) */
typedef enum {
    API2_VERB_GET         = 0x0U,
    API2_VERB_SET         = 0x1U,
    API2_VERB_EXECUTE     = 0x2U,
    API2_VERB_SUBSCRIBE   = 0x3U,
    API2_VERB_UNSUBSCRIBE = 0x4U,
    API2_VERB_START_BULK  = 0x5U,
    API2_VERB_CANCEL_BULK = 0x6U,
    /* 0x7-0xF reserved */
} Api2Verb;

typedef enum {
    API2_CAT_SYSTEM_STATUS = 0x0U,
    API2_CAT_COMMANDS      = 0x1U,
    API2_CAT_CALIBRATIONS  = 0x2U,
    API2_CAT_SETTINGS      = 0x3U,
    API2_CAT_MEASUREMENTS  = 0x4U,
    API2_CAT_TOPIC_GROUPS  = 0x5U,
    API2_CAT_DEBUG_MSGS    = 0x6U,
    API2_CAT_RAW_DATA      = 0x7U,
    API2_CAT_BULK          = 0x8U,
    /* 0x9-0xF reserved */
} Api2Category;

#define API2_OPCODE(verb, cat, res) \
    ((uint16_t)((((uint16_t)(verb) & 0xFU) << 12) \
              | (((uint16_t)(cat)  & 0xFU) << 8)  \
              |  ((uint16_t)(res)  & 0xFFU)))
#define API2_OPCODE_VERB(op)     ((uint8_t)(((op) >> 12) & 0xFU))
#define API2_OPCODE_CATEGORY(op) ((uint8_t)(((op) >> 8)  & 0xFU))
#define API2_OPCODE_RESOURCE(op) ((uint8_t)((op) & 0xFFU))

/* ---------------- status codes (docs/api-v2-spec.md §5) ---------------- */
typedef enum {
    API2_STATUS_OK                = 0x00U,
    API2_STATUS_UNKNOWN_CATEGORY  = 0x01U,
    API2_STATUS_VERB_NOT_VALID    = 0x02U,
    API2_STATUS_UNKNOWN_RESOURCE  = 0x03U,
    API2_STATUS_BAD_CRC           = 0x04U,
    API2_STATUS_BAD_LENGTH        = 0x05U,
    API2_STATUS_BUSY_RESOURCE     = 0x06U,
    API2_STATUS_BUSY_EXCLUSIVE    = 0x07U,
    API2_STATUS_INVALID_PARAMETER = 0x08U,
    API2_STATUS_NOT_SUBSCRIBED    = 0x09U,
    API2_STATUS_NOTHING_TO_CANCEL = 0x0AU,
} Api2Status;

/* ---------------- stage 1 resource opcodes ----------------
 * System status (GET only) and Commands (EXECUTE only) -- see
 * docs/api-reference.md for the full payload contract of each. Resource
 * indices assigned sequentially as implemented (docs/api-v2-spec.md §9,
 * confirmed with the user 2026-08-19 -- not pre-allocated). */
#define API2_OP_SYS_GET_IDENTITY \
    API2_OPCODE(API2_VERB_GET, API2_CAT_SYSTEM_STATUS, 0x00U)
#define API2_OP_SYS_GET_DEVICE_STATE \
    API2_OPCODE(API2_VERB_GET, API2_CAT_SYSTEM_STATUS, 0x01U)
#define API2_OP_CMD_TEST_BEEP \
    API2_OPCODE(API2_VERB_EXECUTE, API2_CAT_COMMANDS, 0x00U)

#endif /* SVC_API_H */
