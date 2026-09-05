#ifndef SVC_TXFRAME_H
#define SVC_TXFRAME_H

#include <stdint.h>
#include <stdbool.h>

/* Per-transport outbound frame FIFO (CLAUDE.md §8.3).
 *
 * One instance backs each API transport's TX path (Services/svc_usb.c,
 * svc_ble.c, svc_uart.c). svc_api.c builds a complete
 * [OPCODE][LEN][PAYLOAD][CRC16] packet and hands it to the transport's
 * send_fn; the transport enqueues it here and a drain pump (run every
 * scheduler tick, and again right after each enqueue) moves one frame at
 * a time onto the wire as the link becomes free. This decouples the
 * dispatcher from a slow or blocked link — a BLE central that stops
 * draining notifications can no longer stall the cooperative scheduler
 * inside a blocking UART write.
 *
 * Storage is a caller-provided byte buffer, used as a ring of
 * length-prefixed frames: [u16 len LE][len bytes][u16 len LE]... Indices
 * wrap by mask, so the buffer size must be a power of two. One byte is
 * kept unused so head == tail unambiguously means "empty".
 *
 * Single-producer / single-consumer, and today both run on the scheduler
 * thread (no ISR touches a ring), so no locking. head/tail are plain
 * uint16_t; if a consumer ever moves into a TX-complete ISR, make them
 * volatile and re-audit.
 *
 * Two-tier admission — the "reserved emergency space" of §8.3:
 *   svc_txframe_push(..., urgent=false)  streaming / subscription pushes.
 *       Accepted only if the frame fits AND at least
 *       SVC_TXFRAME_RESERVE_BYTES stay free afterwards.
 *   svc_txframe_push(..., urgent=true)   command responses and the
 *       overflow-notice path. May consume the reserve; refused only if
 *       the frame genuinely will not fit.
 * So a flood of subscription data can fill the normal region but can
 * never starve the response to the host's next command. A refused push
 * returns false and the caller escalates (drop counter + one WARN log) —
 * it never blocks and never drops already-queued data. */

#define SVC_TXFRAME_RESERVE_BYTES   64U
#define SVC_TXFRAME_LEN_PREFIX      2U

typedef struct {
    uint8_t *buf;
    uint16_t size;    /* power of two */
    uint16_t mask;    /* size - 1 */
    uint16_t head;    /* next write index */
    uint16_t tail;    /* next read index  */
} SvcTxFrame;

/* `storage` must be `size` bytes and `size` must be a power of two >= 128
 * (one max API2 packet + its length prefix + the reserve). */
void     svc_txframe_init(SvcTxFrame *r, uint8_t *storage, uint16_t size);

/* Enqueue one frame. Returns false (nothing queued) if it will not fit
 * under the admission rule for `urgent` — see the header comment. */
bool     svc_txframe_push(SvcTxFrame *r, const uint8_t *data, uint16_t len, bool urgent);

/* Copy the front frame into `dst` WITHOUT removing it; returns its length,
 * or 0 if the FIFO is empty or `maxlen` is smaller than the frame. */
uint16_t svc_txframe_peek(const SvcTxFrame *r, uint8_t *dst, uint16_t maxlen);

/* Remove the front frame. No-op if empty. */
void     svc_txframe_drop_front(SvcTxFrame *r);

/* peek + drop_front. Returns the length copied (0 if empty / too small,
 * in which case nothing is removed). */
uint16_t svc_txframe_pop(SvcTxFrame *r, uint8_t *dst, uint16_t maxlen);

bool     svc_txframe_is_empty(const SvcTxFrame *r);
uint16_t svc_txframe_free_bytes(const SvcTxFrame *r);

/* Discard everything queued — used when a transport's peer goes away and
 * the queued frames are for a connection that no longer exists. */
void     svc_txframe_reset(SvcTxFrame *r);

#endif /* SVC_TXFRAME_H */
