#ifndef SVC_API_CORE_H
#define SVC_API_CORE_H

/* Types shared by the API dispatcher (svc_api.c), the generated tables (svc_api_tables.c, from tools/api_spec.py)
 * and the resource handlers (svc_api_res_*.c). Nothing outside Services/svc_api*.c needs this header; transports and
 * the App layer use svc_api.h.
 *
 * Protocol: docs/api-v3-spec.md.  Resources: docs/api-reference.md (generated). */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

typedef enum {
    API_TRANSPORT_USB  = 0,
    API_TRANSPORT_BLE  = 1,
    API_TRANSPORT_UART = 2,   /* USART3 debug/VCP header -- Services/svc_uart.c */
    API_TRANSPORT_COUNT,
} ApiTransport;

/* ---------------- opcode: [VERB:4][CATEGORY:4][RESOURCE:8] ---------------- */
typedef enum {
    API2_VERB_GET         = 0x0U,
    API2_VERB_SET         = 0x1U,
    API2_VERB_EXECUTE     = 0x2U,
    API2_VERB_SUBSCRIBE   = 0x3U,
    API2_VERB_UNSUBSCRIBE = 0x4U,
    API2_VERB_START_BULK  = 0x5U,
    API2_VERB_CANCEL_BULK = 0x6U,
} Api2Verb;

#define API2_VERB_BIT(v)         ((uint8_t)(1U << (unsigned)(v)))

#define API2_OPCODE(verb, cat, res) \
    ((uint16_t)((((uint16_t)(verb) & 0xFU) << 12) \
              | (((uint16_t)(cat)  & 0xFU) << 8)  \
              |  ((uint16_t)(res)  & 0xFFU)))
#define API2_OPCODE_VERB(op)     ((uint8_t)(((op) >> 12) & 0xFU))
#define API2_OPCODE_CATEGORY(op) ((uint8_t)(((op) >> 8)  & 0xFU))
#define API2_OPCODE_RESOURCE(op) ((uint8_t)((op) & 0xFFU))

/* ---------------- status codes (first payload byte of every response) ---------------- */
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
    API2_STATUS_SERVICE_MODE_REQUIRED = 0x0BU,   /* the resource needs service mode (api-v3-spec.md section 5a) */
} Api2Status;

/* ---------------- packet framing: [OPCODE 2][LEN 2][PAYLOAD LEN][CRC16 2] ---------------- */
#define API2_PACKET_HDR_BYTES   4U
#define API2_PACKET_CRC_BYTES   2U
#define API2_PACKET_MAX_SIZE    128U
/* Largest payload a packet may carry; the status byte of a response takes one of it. */
#define API2_PACKET_MAX_PAYLOAD (API2_PACKET_MAX_SIZE - API2_PACKET_HDR_BYTES - API2_PACKET_CRC_BYTES)
#define API2_RESPONSE_DATA_MAX  (API2_PACKET_MAX_PAYLOAD - 1U)
/* Subscription pushes carry [issue_seq][page] before the data. */
#define API2_PUSH_DATA_MAX      (API2_PACKET_MAX_PAYLOAD - 1U - 2U)

#define API2_INTERVAL_MIN_MS    50U
#define API2_INTERVAL_MAX_MS    3600000U

/* ---------------- one request, as a resource handler sees it ---------------- */
typedef struct {
    ApiTransport   t;
    uint8_t        verb;            /* Api2Verb */
    uint8_t        res;             /* resource id */
    const uint8_t *in;              /* request payload (SET / EXECUTE / SUBSCRIBE), in_len bytes, CRC-checked */
    uint16_t       in_len;
    uint8_t       *out;             /* response data after the status byte (GET / EXECUTE), out_cap bytes */
    uint16_t       out_cap;
    uint16_t       out_len;         /* handler sets */
    void         (*after_reply)(void);   /* handler may set: run once the response was handed to the transport
                                          (power off, reboot: the answer must get out first) */
} ApiCall;

/* Copies a packed payload struct into the response. */
#define API2_REPLY(call, structp)                                         \
    do { _Static_assert(sizeof(*(structp)) <= API2_RESPONSE_DATA_MAX, "response too large"); \
         memcpy((call)->out, (structp), sizeof(*(structp)));                  \
         (call)->out_len = (uint16_t)sizeof(*(structp)); } while (0)

typedef struct ApiResource ApiResource;
typedef Api2Status (*ApiHandler)(const ApiResource *r, ApiCall *c);

/* ---------------- subscriptions ---------------- */
typedef struct {
    bool     active;
    uint8_t  issue_seq;
    uint32_t interval_ms;           /* interval subscriptions */
    uint32_t last_push_ms;
    uint32_t a, b;                  /* free scratch of event resources (cursor, last seen state, ...) */
} ApiSub;

typedef enum { API2_SUB_NONE = 0, API2_SUB_INTERVAL = 1, API2_SUB_EVENT = 2 } Api2SubKind;

/* Event-driven subscription: the resource decides when something is worth pushing.
 *  start   validates the SUBSCRIBE request and arms whatever is needed (may be called for an already active
 *          subscription: be idempotent); OK activates the slot.
 *  stop    undo start; also called when the transport disconnects.
 *  poll    called every scheduler tick (up to `burst` times while the transport has transmit headroom) for an
 *          active slot; fills out[0..*len) and returns true to push one frame, false when nothing is due. */
typedef struct {
    Api2Status (*start)(const ApiResource *r, ApiTransport t, ApiSub *s, const uint8_t *in, uint16_t in_len);
    void       (*stop)(const ApiResource *r, ApiTransport t, ApiSub *s);
    bool       (*poll)(const ApiResource *r, ApiTransport t, ApiSub *s, uint8_t *out, uint16_t *len);
} ApiEventOps;

/* ---------------- resource tables (generated into svc_api_tables.c) ---------------- */
struct ApiResource {
    uint8_t            id;
    uint8_t            verbs;        /* mask of API2_VERB_BIT() */
    uint8_t            sub;          /* Api2SubKind */
    uint8_t            sub_in_len;   /* SUBSCRIBE request length */
    uint16_t           in_min, in_max;   /* request length of SET / EXECUTE */
    ApiHandler         handler;      /* GET / SET / EXECUTE / START_BULK / CANCEL_BULK (c->verb) */
    const ApiEventOps *ev;           /* event subscriptions */
    const void        *ctx;          /* ApiFieldDesc of a field resource */
    uint8_t            slot;         /* index into the per-transport subscription table */
    uint8_t            burst;        /* event pushes per tick */
    uint8_t            flags;        /* API2_RES_F_* */
};
#define API2_RES_F_SERVICE  0x01U   /* SET / EXECUTE need service mode */

typedef struct {
    uint8_t            id;
    uint8_t            verbs;        /* verbs the category accepts at all */
    const ApiResource *res;
    uint8_t            count;
} ApiCategory;

/* A resource backed by one DeviceSettings member; handled by api_field_handler (svc_api_fields.c). */
typedef struct {
    uint8_t  res;
    uint8_t  size;                   /* 1, 2 or 4 */
    bool     is_signed;
    size_t   offset;                 /* offsetof(DeviceSettings, member) */
    int64_t  lo, hi;
    bool   (*check)(uint8_t res, int64_t value);   /* optional cross-field rule */
    void   (*after)(void);                          /* optional, after a successful save */
    bool     notify;                                /* call the registered settings-changed hook */
} ApiFieldDesc;

Api2Status api_field_handler(const ApiResource *r, ApiCall *c);

/* ---------------- services of the core to the resource handlers ---------------- */
bool     api_transport_connected(ApiTransport t);
bool     api_transport_ready(ApiTransport t);                 /* TX ring has headroom for a full frame */
/* Sends a non-urgent frame [status OK][data...] under `opcode` (subscription pushes, bulk chunks). */
void     api_send_push(ApiTransport t, uint16_t opcode, const uint8_t *data, uint16_t len);

uint16_t svc_api_rx_malformed(void);
void     svc_api_clear_counters(void);

/* ---------------- implemented by the resource side (svc_api_res_*.c) ---------------- */
void api_res_update(void);                      /* once per svc_api_update(): bulk pump, procedure follow-ups */
void api_res_request_seen(void);                /* a valid request reached the dispatcher: API activity (service-mode timeout) */
bool api_service_unlocked(void);                /* service mode is active */
void api_res_transport_disconnected(ApiTransport t);

#endif /* SVC_API_CORE_H */
