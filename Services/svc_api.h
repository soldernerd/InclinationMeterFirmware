#ifndef SVC_API_H
#define SVC_API_H

#include <stdint.h>
#include <stdbool.h>
#include "svc_api_core.h"

/* Device API v3 -- transport-facing and App-facing entry points. The protocol is docs/api-v3-spec.md, the resources
 * docs/api-reference.md (both generated or derived from tools/api_spec.py). Transports (svc_usb.c / svc_ble.c /
 * svc_uart.c) never need more than what is declared here. */

/* `urgent` distinguishes a direct response to a host request (true -- may use a transport's reserved TX space) from a
 * subscription / stream push (false -- must leave the reserve free so a response can always get out). See
 * Services/svc_txframe.h. */
typedef void (*ApiSendFn)(const uint8_t *data, uint16_t len, bool urgent);

/* Optional per-transport back-pressure hook: true when the transport's TX ring can take at least one more full-size
 * non-urgent frame without eating the reserve. Event pushes and the bulk pump consult it; a transport that registers
 * none is treated as always ready. */
typedef bool (*ApiReadyFn)(void);

void svc_api_init(void);
void svc_api_update(void);                    /* scheduler hook, every tick: event pushes, bulk pump, procedures */
void svc_api_subscriptions_update(void);      /* scheduler hook, every tick: due interval subscriptions */

void svc_api_register_transport(ApiTransport t, ApiSendFn send_fn);
void svc_api_register_transport_ready(ApiTransport t, ApiReadyFn ready_fn);

/* Called after a host SET of a Settings resource has been persisted, so an upper layer can re-apply anything derived
 * from DeviceSettings (App wires app_scheduler_reload_periods here). Keeps svc_api free of App-layer includes
 * (CLAUDE.md 8.1 -- dependencies flow downward). */
typedef void (*ApiSettingsChangedFn)(void);
void svc_api_register_settings_changed(ApiSettingsChangedFn fn);
void svc_api_settings_changed(void);          /* used by the field handler */

void svc_api_connected(ApiTransport t);
void svc_api_disconnected(ApiTransport t);

/* One complete CRC-framed packet received from `t`. */
void svc_api_receive(ApiTransport t, const uint8_t *data, uint16_t len);

/* Byte-stream transports (BLE, UART): feed bytes one at a time; a complete packet is dispatched and the reassembler
 * resets. svc_api_reassembler_check_timeout() abandons a stalled partial packet. */
typedef struct {
    uint8_t  buf[API2_PACKET_MAX_SIZE];
    uint16_t pos;
    uint32_t started_ms;
} ApiByteReassembler;

void svc_api_reassembler_feed_byte(ApiTransport t, ApiByteReassembler *r, uint8_t b);
void svc_api_reassembler_check_timeout(ApiByteReassembler *r, uint32_t timeout_ms);

#endif /* SVC_API_H */
