#ifndef SVC_LOG_H
#define SVC_LOG_H

#include <stdint.h>
#include <stdbool.h>
#include "svc_api.h"     /* Api2LogSeverity */
#include "config.h"      /* SVC_LOG_MSG_MAX */

/* In-RAM ring of recent log lines, drained by the API v2 Debug-messages
 * stream (svc_api.c, category 0x6) so a host can watch the device log
 * live over USB/BLE. Not a printf-to-UART -- there's no wired console.
 *
 * Layering: App/ and Services/ call svc_log()/svc_logf() freely. HAL_App/
 * and Drivers_App/ still escalate via g_system_state counters (CLAUDE.md
 * 8.1) rather than logging directly. */

void svc_log_init(void);

void svc_log(Api2LogSeverity sev, const char *msg);
void svc_logf(Api2LogSeverity sev, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

/* API-facing drain. `cursor` is the caller's "last delivered seq" (start
 * at 0). Returns true and fills the out params with the next entry newer
 * than *cursor whose severity >= min_sev, advancing *cursor to it. Returns
 * false when the caller is caught up. If the ring wrapped past *cursor,
 * skips ahead to the oldest still-held entry (lines in between are lost --
 * that's the spec's "no chunk-level retransmission" tradeoff for a stream). */
bool svc_log_drain(uint32_t *cursor, Api2LogSeverity min_sev,
                   Api2LogSeverity *out_sev, char *out_msg, uint8_t *out_len);

#endif /* SVC_LOG_H */
