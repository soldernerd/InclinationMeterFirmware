#include "svc_displacement.h"
#include "svc_displacement_internal.h"
#include "drv_ads131m04.h"
#include "config.h"

/* The continuous phasor batch stream (API Topics 0x05), split out of svc_displacement.c: a tap
 * on the running measurement that copies every batch's raw phasors into a FIFO the API drains. */

/* Continuous phasor batch stream: FIFO filled by svc_displacement_update(),
 * drained by the API layer (svc_displacement.h "Continuous phasor batch
 * stream"). Both sides run in task context, so no locking is needed. */
#define PSTREAM_MASK (DISPLACEMENT_PHASOR_STREAM_DEPTH - 1U)
#if (DISPLACEMENT_PHASOR_STREAM_DEPTH & PSTREAM_MASK) != 0
#error "DISPLACEMENT_PHASOR_STREAM_DEPTH must be a power of two"
#endif
static DisplacementPhasorEntry s_pstream[DISPLACEMENT_PHASOR_STREAM_DEPTH];
static uint16_t s_pstream_head   = 0;
static uint16_t s_pstream_tail   = 0;
static uint16_t s_pstream_drops  = 0;
bool            g_disp_pstream_active = false;

/* Queues one completed batch for the continuous stream. A full FIFO drops
 * the NEWEST batch (counted) -- the consumer sees the loss as a seq jump. */
void disp_pstream_store(const MathBatchSums *s, uint16_t seq)
{
    uint16_t next = (uint16_t)((s_pstream_head + 1U) & PSTREAM_MASK);
    if (next == s_pstream_tail) {
        if (s_pstream_drops < UINT16_MAX) s_pstream_drops++;
        return;
    }
    DisplacementPhasorEntry *e = &s_pstream[s_pstream_head];
    e->iB  = (float)s->iB;  e->qB  = (float)s->qB;
    e->iA  = (float)s->iA;  e->qA  = (float)s->qA;
    e->iS1 = (float)s->iS1; e->qS1 = (float)s->qS1;
    e->iS2 = (float)s->iS2; e->qS2 = (float)s->qS2;
    e->seq = seq;
    s_pstream_head = next;
}

void disp_pstream_clear_drops(void)
{
    s_pstream_drops = 0;
}

DrvStatus svc_displacement_phasor_stream_begin(void)
{
    if (g_disp_pstream_active || g_disp_cap_active) {
        return DRV_ERR_NOT_READY;
    }
    s_pstream_head   = 0;
    s_pstream_tail   = 0;
    s_pstream_drops  = 0;
    g_disp_pstream_active = true;   /* svc_displacement_update() now copies every batch into the FIFO */
    if (!drv_ads131m04_is_running()) {
        /* Not measuring (e.g. stopped over the API): start the real measurement, so the
         * demod state is initialised properly and the stream is a tap on it, as always. */
        DrvStatus rc = svc_displacement_start();
        if (rc != DRV_OK) {
            g_disp_pstream_active = false;
            return rc;
        }
    }
    return DRV_OK;
}

bool svc_displacement_phasor_stream_active(void)
{
    return g_disp_pstream_active;
}

void svc_displacement_phasor_stream_end(void)
{
    g_disp_pstream_active = false;   /* the measurement keeps running; only the tap is removed */
}

bool svc_displacement_phasor_stream_peek(DisplacementPhasorEntry *out)
{
    if (s_pstream_tail == s_pstream_head) return false;
    *out = s_pstream[s_pstream_tail];
    return true;
}

void svc_displacement_phasor_stream_consume(void)
{
    if (s_pstream_tail != s_pstream_head) {
        s_pstream_tail = (uint16_t)((s_pstream_tail + 1U) & PSTREAM_MASK);
    }
}

uint16_t svc_displacement_phasor_stream_drops(void)
{
    return s_pstream_drops;
}
