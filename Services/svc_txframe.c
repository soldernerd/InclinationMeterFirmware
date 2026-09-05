#include "svc_txframe.h"
#include <string.h>

/* See svc_txframe.h for the design. All index arithmetic is mod `size`
 * via `mask`; `used` can never reach `size` because push() always leaves
 * at least one byte free (the empty/full disambiguator). */

static uint16_t used_bytes(const SvcTxFrame *r)
{
    return (uint16_t)((r->head - r->tail) & r->mask);
}

static void ring_write(SvcTxFrame *r, const uint8_t *data, uint16_t len)
{
    uint16_t first = (uint16_t)(r->size - r->head);
    if (first >= len) {
        memcpy(&r->buf[r->head], data, len);
    } else {
        memcpy(&r->buf[r->head], data, first);
        memcpy(&r->buf[0], data + first, (size_t)(len - first));
    }
    r->head = (uint16_t)((r->head + len) & r->mask);
}

static void ring_read(const SvcTxFrame *r, uint16_t from, uint8_t *dst, uint16_t len)
{
    uint16_t first = (uint16_t)(r->size - from);
    if (first >= len) {
        memcpy(dst, &r->buf[from], len);
    } else {
        memcpy(dst, &r->buf[from], first);
        memcpy(dst + first, &r->buf[0], (size_t)(len - first));
    }
}

static uint16_t front_len(const SvcTxFrame *r)
{
    uint16_t lo = r->buf[r->tail];
    uint16_t hi = r->buf[(uint16_t)((r->tail + 1U) & r->mask)];
    return (uint16_t)(lo | (hi << 8));
}

void svc_txframe_init(SvcTxFrame *r, uint8_t *storage, uint16_t size)
{
    r->buf  = storage;
    r->size = size;
    r->mask = (uint16_t)(size - 1U);
    r->head = 0;
    r->tail = 0;
}

uint16_t svc_txframe_free_bytes(const SvcTxFrame *r)
{
    return (uint16_t)(r->size - used_bytes(r) - 1U);
}

bool svc_txframe_is_empty(const SvcTxFrame *r)
{
    return r->head == r->tail;
}

bool svc_txframe_push(SvcTxFrame *r, const uint8_t *data, uint16_t len, bool urgent)
{
    if (data == 0 || len == 0) {
        return false;
    }
    /* A single frame must never need more than half the ring — keeps one
     * oversized frame from being able to fill the reserve on its own. */
    uint16_t need = (uint16_t)(SVC_TXFRAME_LEN_PREFIX + len);
    if (need > (uint16_t)(r->size / 2U)) {
        return false;
    }

    uint16_t want = urgent ? need : (uint16_t)(need + SVC_TXFRAME_RESERVE_BYTES);
    if (svc_txframe_free_bytes(r) < want) {
        return false;
    }

    uint8_t prefix[SVC_TXFRAME_LEN_PREFIX] = {
        (uint8_t)(len & 0xFFU),
        (uint8_t)((len >> 8) & 0xFFU),
    };
    ring_write(r, prefix, SVC_TXFRAME_LEN_PREFIX);
    ring_write(r, data, len);
    return true;
}

uint16_t svc_txframe_peek(const SvcTxFrame *r, uint8_t *dst, uint16_t maxlen)
{
    if (dst == 0 || svc_txframe_is_empty(r)) {
        return 0;
    }
    uint16_t len = front_len(r);
    if (len == 0 || len > maxlen) {
        return 0;
    }
    ring_read(r, (uint16_t)((r->tail + SVC_TXFRAME_LEN_PREFIX) & r->mask), dst, len);
    return len;
}

void svc_txframe_drop_front(SvcTxFrame *r)
{
    if (svc_txframe_is_empty(r)) {
        return;
    }
    uint16_t len = front_len(r);
    r->tail = (uint16_t)((r->tail + SVC_TXFRAME_LEN_PREFIX + len) & r->mask);
}

uint16_t svc_txframe_pop(SvcTxFrame *r, uint8_t *dst, uint16_t maxlen)
{
    uint16_t len = svc_txframe_peek(r, dst, maxlen);
    if (len != 0) {
        svc_txframe_drop_front(r);
    }
    return len;
}

void svc_txframe_reset(SvcTxFrame *r)
{
    r->head = 0;
    r->tail = 0;
}
