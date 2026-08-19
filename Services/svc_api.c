#include "svc_api.h"
#include "svc_battery.h"
#include "drv_buzzer.h"
#include "math_crc.h"
#include "hal_systick.h"
#include "system_state.h"
#include "config.h"
#include "app_version.h"
#include <string.h>

/* Device API v2 (WP11) -- breaking replacement of the v1 protocol
 * previously implemented here. See docs/api-v2-spec.md for the design
 * rationale and docs/api-reference.md for the host-facing contract.
 *
 * Stage 1 of the implementation plan (2026-08-19 branching-strategy
 * discussion): core packet format + dispatcher (spec §2, §3, §5, §6),
 * proven end-to-end with the two simplest categories -- System status
 * (GET only) and Commands (EXECUTE only). No SUBSCRIBE/bulk machinery
 * yet; svc_api_update() is a no-op placeholder until stage 2 needs it.
 *
 * On-the-wire packet: [OPCODE 2B LE][LEN 2B LE][PAYLOAD 0..LEN][CRC16 2B
 * LE], no padding, total 6+LEN. Every response echoes the request's
 * opcode; a full status byte (Api2Status) is always the first byte of
 * the response payload, followed by resource-specific data (if any) only
 * when status is API2_STATUS_OK. */

#define MAX_PAYLOAD (API2_PACKET_MAX_SIZE - API2_PACKET_HDR_BYTES - API2_PACKET_CRC_BYTES)

/* ---------------- payload structs ---------------- */

typedef struct {
    uint8_t fw_major;
    uint8_t fw_minor;
    uint8_t fw_patch;
    char    product_str[16];
    char    serial_str[8];
} __attribute__((packed)) Api2IdentityPayload;

typedef struct {
    uint8_t  battery_state;     /* battery_state_t (Services/svc_battery.h) */
    uint8_t  battery_soc_pct;
    uint16_t battery_mv;
    uint8_t  usb_connected;
    uint8_t  ble_connected;
    uint8_t  calibration_valid;
} __attribute__((packed)) Api2DeviceStatePayload;

/* +1 for the status byte every response payload is prefixed with. */
_Static_assert(sizeof(Api2IdentityPayload)    + 1U <= MAX_PAYLOAD, "IDENTITY response too large");
_Static_assert(sizeof(Api2DeviceStatePayload) + 1U <= MAX_PAYLOAD, "DEVICE_STATE response too large");

/* ---------------- per-transport state ---------------- */

typedef struct {
    bool      connected;
    ApiSendFn send_fn;
} ApiTransportState;

static ApiTransportState s_t[API_TRANSPORT_COUNT];

/* ---------------- helpers ---------------- */

/* Fill `dst` with up to `cap` bytes of `src`, zero-padding any remainder.
 * No NUL guarantee -- fields are fixed-width and the host parses by
 * length, not by C-string termination (same convention v1 used). */
static void copy_fixed(char *dst, const char *src, size_t cap)
{
    size_t n = 0;
    while (n < cap && src[n] != '\0') { n++; }
    memcpy(dst, src, n);
    if (n < cap) {
        memset(dst + n, 0, cap - n);
    }
}

/* CLAUDE.md 7.6 escalation for a request/frame this file can't respond to
 * or dispatch -- see SystemState.api_rx_malformed_count's own comment for
 * why this is a counter, not a DBG_PRINT call. */
static void note_malformed(void)
{
    if (g_system_state.api_rx_malformed_count < UINT16_MAX) {
        g_system_state.api_rx_malformed_count++;
    }
}

static void send_response(ApiTransport t, uint16_t opcode, Api2Status status,
                           const uint8_t *data, uint16_t data_len)
{
    if (t >= API_TRANSPORT_COUNT)             return;
    if (!s_t[t].connected || !s_t[t].send_fn) return;

    uint16_t payload_len = (uint16_t)(1U + data_len);   /* status byte + resource data */
    if (payload_len > MAX_PAYLOAD) {
        /* Shouldn't happen at stage-1's payload sizes, but this guards
         * every future category's response-building too -- a resource
         * handler bug or a legitimately-oversized response both land
         * here. */
        note_malformed();
        return;
    }

    uint8_t buf[API2_PACKET_MAX_SIZE];
    buf[0] = (uint8_t)(opcode & 0xFFU);
    buf[1] = (uint8_t)((opcode >> 8) & 0xFFU);
    buf[2] = (uint8_t)(payload_len & 0xFFU);
    buf[3] = (uint8_t)((payload_len >> 8) & 0xFFU);
    buf[4] = (uint8_t)status;
    if (data && data_len) {
        memcpy(&buf[5], data, data_len);
    }

    uint16_t before_crc = (uint16_t)(API2_PACKET_HDR_BYTES + payload_len);
    uint16_t crc = math_crc16(buf, before_crc);
    buf[before_crc + 0U] = (uint8_t)(crc & 0xFFU);
    buf[before_crc + 1U] = (uint8_t)((crc >> 8) & 0xFFU);

    s_t[t].send_fn(buf, (uint16_t)(before_crc + API2_PACKET_CRC_BYTES));
}

/* Checks CRC over the full received frame. Called only after category/
 * verb/resource have already been confirmed valid (dispatch validation
 * order, docs/api-v2-spec.md §3.4 steps 1-3 before step 4) -- so CRC
 * cycles are only spent on requests that are worth dispatching in the
 * first place, and the status-code priority matches §5/§3.4 exactly
 * (a corrupted-but-structurally-plausible opcode is diagnosed by its
 * apparent category/verb/resource before CRC gets a say). This does NOT
 * mean an invalid request ever actually executes on bad data -- dispatch
 * still stops here on mismatch, before any resource handler runs. */
static bool check_crc(ApiTransport t, uint16_t opcode, const uint8_t *frame, uint16_t paylen)
{
    uint16_t before_crc = (uint16_t)(API2_PACKET_HDR_BYTES + paylen);
    uint16_t calc_crc = math_crc16(frame, before_crc);
    uint16_t got_crc  = (uint16_t)(frame[before_crc + 0U] | (frame[before_crc + 1U] << 8));
    if (calc_crc != got_crc) {
        send_response(t, opcode, API2_STATUS_BAD_CRC, 0, 0);
        return false;
    }
    return true;
}

/* ---------------- System status (GET only) ---------------- */

static void fill_identity(Api2IdentityPayload *p)
{
    memset(p, 0, sizeof *p);
    p->fw_major = (uint8_t)FW_VERSION_MAJOR;
    p->fw_minor = (uint8_t)FW_VERSION_MINOR;
    p->fw_patch = (uint8_t)FW_VERSION_PATCH;
    copy_fixed(p->product_str, USB_PRODUCT_STR, sizeof p->product_str);
    copy_fixed(p->serial_str,  USB_SERIAL_STR,  sizeof p->serial_str);
}

static void fill_device_state(Api2DeviceStatePayload *p)
{
    p->battery_state     = (uint8_t)svc_battery_get_state();
    p->battery_soc_pct   = svc_battery_get_soc_pct();
    p->battery_mv        = svc_battery_get_vbat_mv();
    p->usb_connected     = g_system_state.usb_connected     ? 1U : 0U;
    p->ble_connected     = g_system_state.ble_connected     ? 1U : 0U;
    p->calibration_valid = g_system_state.calibration_valid ? 1U : 0U;
}

static void dispatch_system_status(ApiTransport t, uint16_t opcode, uint8_t verb,
                                    uint8_t res, const uint8_t *frame, uint16_t paylen)
{
    if (verb != API2_VERB_GET) {
        send_response(t, opcode, API2_STATUS_VERB_NOT_VALID, 0, 0);
        return;
    }
    if (res != 0x00U && res != 0x01U) {
        send_response(t, opcode, API2_STATUS_UNKNOWN_RESOURCE, 0, 0);
        return;
    }
    if (!check_crc(t, opcode, frame, paylen)) return;
    if (paylen != 0U) {
        send_response(t, opcode, API2_STATUS_BAD_LENGTH, 0, 0);
        return;
    }

    if (res == 0x00U) {
        Api2IdentityPayload p;
        fill_identity(&p);
        send_response(t, opcode, API2_STATUS_OK, (const uint8_t *)&p, sizeof p);
    } else {
        Api2DeviceStatePayload p;
        fill_device_state(&p);
        send_response(t, opcode, API2_STATUS_OK, (const uint8_t *)&p, sizeof p);
    }
}

/* ---------------- Commands (EXECUTE only) ---------------- */

static void dispatch_commands(ApiTransport t, uint16_t opcode, uint8_t verb,
                               uint8_t res, const uint8_t *frame, uint16_t paylen)
{
    if (verb != API2_VERB_EXECUTE) {
        send_response(t, opcode, API2_STATUS_VERB_NOT_VALID, 0, 0);
        return;
    }
    if (res != 0x00U) {
        send_response(t, opcode, API2_STATUS_UNKNOWN_RESOURCE, 0, 0);
        return;
    }
    if (!check_crc(t, opcode, frame, paylen)) return;
    if (paylen != 0U) {
        send_response(t, opcode, API2_STATUS_BAD_LENGTH, 0, 0);
        return;
    }

    /* TEST_BEEP -- deliberately chosen as the first EXECUTE resource for
     * being safe/self-contained/side-effect-free beyond a short beep, to
     * prove the verb/dispatch mechanics without coupling stage 1 to
     * anything stateful (measurement triggering belongs to Measurements
     * in stage 2 per the spec's own Commands-category framing; charging/
     * sleep control via HAL_App/hal_power.c is real future Commands
     * content but not needed to prove this stage). */
    drv_buzzer_beep(BUZZER_TONE_CLICK, 100U);
    send_response(t, opcode, API2_STATUS_OK, 0, 0);
}

/* ---------------- dispatch ---------------- */

static void dispatch(ApiTransport t, uint16_t opcode, const uint8_t *frame, uint16_t paylen)
{
    uint8_t verb = API2_OPCODE_VERB(opcode);
    uint8_t cat  = API2_OPCODE_CATEGORY(opcode);
    uint8_t res  = API2_OPCODE_RESOURCE(opcode);

    switch (cat) {
        case API2_CAT_SYSTEM_STATUS:
            dispatch_system_status(t, opcode, verb, res, frame, paylen);
            return;
        case API2_CAT_COMMANDS:
            dispatch_commands(t, opcode, verb, res, frame, paylen);
            return;
        default:
            /* Every other category (0x2-0x8 defined but not yet built in
             * this firmware, or 0x9-0xF genuinely undefined) is
             * UNKNOWN_CATEGORY -- docs/api-v2-spec.md §7's philosophy is
             * that a host knows from versioned documentation what a given
             * firmware build supports, so "not implemented yet" and "not
             * a real category" are the same answer from the wire's point
             * of view. Doesn't reach CRC/length checks -- category
             * existence is validation step 1 (§3.4), before everything
             * else, same as verb/resource above. */
            send_response(t, opcode, API2_STATUS_UNKNOWN_CATEGORY, 0, 0);
            return;
    }
}

/* ---------------- public API ---------------- */

void svc_api_init(void)
{
    memset(s_t, 0, sizeof s_t);
}

void svc_api_register_transport(ApiTransport t, ApiSendFn send_fn)
{
    if (t >= API_TRANSPORT_COUNT) return;
    s_t[t].send_fn = send_fn;
}

void svc_api_connected(ApiTransport t)
{
    if (t >= API_TRANSPORT_COUNT) return;
    s_t[t].connected = true;
}

void svc_api_disconnected(ApiTransport t)
{
    if (t >= API_TRANSPORT_COUNT) return;
    s_t[t].connected = false;
}

void svc_api_receive(ApiTransport t, const uint8_t *data, uint16_t len)
{
    if (t >= API_TRANSPORT_COUNT) return;
    if (data == 0 || len < API2_PACKET_HDR_BYTES + API2_PACKET_CRC_BYTES) {
        note_malformed();
        return;
    }

    uint16_t opcode = (uint16_t)(data[0] | ((uint16_t)data[1] << 8));
    uint16_t paylen = (uint16_t)(data[2] | ((uint16_t)data[3] << 8));

    if ((uint32_t)API2_PACKET_HDR_BYTES + paylen + API2_PACKET_CRC_BYTES > len) {
        /* Declared LEN doesn't match what actually arrived -- can't trust
         * CRC math against a buffer we might read past, and can't tell
         * corrupt-LEN from truncated-delivery apart. Drop rather than
         * echo a possibly-bogus opcode with a guessed status -- mirrors
         * v1's equivalent short-read guard. */
        note_malformed();
        return;
    }

    dispatch(t, opcode, data, paylen);
}

void svc_api_reassembler_feed_byte(ApiTransport t, ApiByteReassembler *r, uint8_t b)
{
    if (r->pos == 0) {
        r->started_ms = hal_systick_get_ms();
    }
    if (r->pos < API2_PACKET_MAX_SIZE) {
        r->buf[r->pos++] = b;
    }

    /* As soon as we have the 4-byte header (opcode + len) we know how big
     * the packet is. */
    if (r->pos >= API2_PACKET_HDR_BYTES) {
        uint16_t paylen = (uint16_t)(r->buf[2] | ((uint16_t)r->buf[3] << 8));
        /* uint32_t, not uint16_t -- a corrupted paylen near 0xFFFF (a
         * real risk: this is exactly why byte-stream transports need
         * this reassembler, since wired UART has no link-layer CRC at
         * all) would otherwise overflow-wrap the sum before the ">
         * API2_PACKET_MAX_SIZE" check below ever saw it, e.g. paylen=
         * 0xFFFA gives a true total of 65536 which truncates to 0 and
         * silently defeats this guard. Cast before adding, matching
         * svc_api_receive()'s equivalent (already-correct) guard. */
        uint32_t total = (uint32_t)API2_PACKET_HDR_BYTES + paylen + API2_PACKET_CRC_BYTES;
        if (total > API2_PACKET_MAX_SIZE) {
            /* Garbage, or a packet bigger than this build's reassembly
             * buffer supports (see API2_PACKET_MAX_SIZE's own comment on
             * deferred multi-frame chaining) -- drop either way. */
            note_malformed();
            r->pos = 0;
        } else if (r->pos >= total) {
            /* total is verified <= API2_PACKET_MAX_SIZE (128) above, so
             * this narrowing back to uint16_t is safe. */
            svc_api_receive(t, r->buf, (uint16_t)total);
            r->pos = 0;
        }
    }
}

void svc_api_reassembler_check_timeout(ApiByteReassembler *r, uint32_t timeout_ms)
{
    if (r->pos > 0 && (hal_systick_get_ms() - r->started_ms) > timeout_ms) {
        r->pos = 0;
    }
}

void svc_api_update(void)
{
    /* Nothing to poll yet at stage 1 -- GET/EXECUTE are synchronous
     * request/response with no periodic push, and there are no
     * subscriptions or bulk transfers to service. Kept as a scheduler-
     * callable hook (matches every other svc_*_update()) for stage 2's
     * subscription delivery. */
}
