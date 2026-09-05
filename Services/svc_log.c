#include "svc_log.h"
#include <string.h>
#include <stdarg.h>
#include <stdio.h>

/* Power-of-two ring. seq is a monotonic 32-bit counter (never wraps in
 * any realistic runtime); slot i holds the entry with seq % SVC_LOG_SLOTS
 * == i once seq >= SVC_LOG_SLOTS. An entry with seq == 0 is "empty".  */
#define SVC_LOG_SLOTS   16U

typedef struct {
    uint32_t seq;
    uint8_t  sev;
    uint8_t  len;
    char     msg[SVC_LOG_MSG_MAX];
} LogEntry;

static LogEntry        s_ring[SVC_LOG_SLOTS];
static volatile uint32_t s_next_seq;

void svc_log_init(void)
{
    memset(s_ring, 0, sizeof s_ring);
    s_next_seq = 1U;   /* seq 0 reserved as "empty" / "nothing delivered yet" */
}

void svc_log(Api2LogSeverity sev, const char *msg)
{
    if (msg == 0) {
        return;
    }
    uint32_t seq = s_next_seq++;
    LogEntry *e = &s_ring[seq % SVC_LOG_SLOTS];

    uint8_t n = 0;
    while (n < SVC_LOG_MSG_MAX && msg[n] != '\0') {
        n++;
    }
    memcpy(e->msg, msg, n);
    e->len = n;
    e->sev = (uint8_t)sev;
    e->seq = seq;          /* set last: a concurrent drain sees a complete entry */
}

void svc_logf(Api2LogSeverity sev, const char *fmt, ...)
{
    char buf[SVC_LOG_MSG_MAX + 1U];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0) {
        return;
    }
    buf[SVC_LOG_MSG_MAX] = '\0';
    svc_log(sev, buf);
}

bool svc_log_drain(uint32_t *cursor, Api2LogSeverity min_sev,
                   Api2LogSeverity *out_sev, char *out_msg, uint8_t *out_len)
{
    if (cursor == 0) {
        return false;
    }
    uint32_t written = s_next_seq;                 /* one past the newest */
    uint32_t oldest  = (written > SVC_LOG_SLOTS) ? (written - SVC_LOG_SLOTS) : 1U;

    /* If we fell behind the ring, jump to the oldest still-held entry. */
    uint32_t from = *cursor + 1U;
    if (from < oldest) {
        from = oldest;
    }

    for (uint32_t seq = from; seq < written; seq++) {
        LogEntry *e = &s_ring[seq % SVC_LOG_SLOTS];
        if (e->seq != seq) {
            continue;                              /* slot already overwritten */
        }
        if (e->sev < (uint8_t)min_sev) {
            *cursor = seq;                         /* skip, but count as seen */
            continue;
        }
        if (out_sev) *out_sev = (Api2LogSeverity)e->sev;
        if (out_len) *out_len = e->len;
        if (out_msg) memcpy(out_msg, e->msg, e->len);
        *cursor = seq;
        return true;
    }
    return false;
}
