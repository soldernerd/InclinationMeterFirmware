#ifndef SVC_SERVICE_H
#define SVC_SERVICE_H

#include <stdint.h>
#include <stdbool.h>

/* Service mode: a deliberate, temporary unlock for the API resources that can damage a calibration or the instrument
 * (calibration writes, factory defaults, DFU, the zero calibration, the power/pin test commands; the list is the
 * `service=True` flag in tools/api_spec.py). Reading is never gated.
 *
 * It can be entered ONLY on the instrument (SETTINGS screen), so a stranger's phone cannot unlock it. It ends
 *   - when someone leaves it (the same menu row, or API Commands SERVICE_END),
 *   - after SERVICE_MODE_IDLE_TIMEOUT_MS (config.h, 10 minutes) without any API request,
 *   - when the instrument goes to sleep (and Standby loses RAM anyway).
 * Nothing about it is persistent. Logic is time-driven through hal_systick; host-tested (tests/test_svc_service.c). */

void svc_service_init(void);
bool svc_service_active(void);

void svc_service_enter(void);               /* local UI only */
void svc_service_leave(void);               /* any source; a no-op if not active */

void svc_service_note_api_activity(void);   /* a valid API request arrived: restarts the idle timeout */
void svc_service_update(void);              /* every tick */

uint16_t svc_service_seconds_left(void);    /* until the idle timeout; 0 when not active */

#endif /* SVC_SERVICE_H */
