#ifndef HAL_RTC_H
#define HAL_RTC_H

#include <stdint.h>
#include <stdbool.h>
#include "drv_common.h"

/* Thin wrapper over the CubeMX RTC (Core/Src/rtc.c, `hrtc`, clocked from
 * the LSE 32.768 kHz crystal Y1, 24-hour format). MX_RTC_Init() has
 * already run in main() before any of this is called.
 *
 * The calendar keeps running through STM32 Standby (the backup domain
 * stays powered) and a warm reset, but is lost on a full power removal
 * (no VBAT coin cell on this board). "Has it ever been set" is answered
 * by the RTC's own INITS flag, which survives Standby the same way. */

typedef struct {
    uint16_t year;      /* full year, e.g. 2026 (valid 2000..2099) */
    uint8_t  month;     /* 1..12 */
    uint8_t  day;       /* 1..31 */
    uint8_t  weekday;   /* 1..7, Mon..Sun — output only; recomputed on set */
    uint8_t  hour;      /* 0..23 */
    uint8_t  minute;    /* 0..59 */
    uint8_t  second;    /* 0..59 */
} rtc_datetime_t;

void      hal_rtc_init(void);

/* Reads the live calendar. Reads TR before DR (the STM32 shadow-register
 * unlock order). `out` is always fully populated. */
void      hal_rtc_get(rtc_datetime_t *out);

/* Sets the calendar. `weekday` in the argument is ignored — it is
 * recomputed from the date. Returns DRV_ERR_INVALID for an out-of-range
 * field, DRV_ERR_COMM if the HAL set call fails, DRV_OK otherwise. */
DrvStatus hal_rtc_set(const rtc_datetime_t *in);

/* True once the calendar has been set at least once since the backup
 * domain last lost power (RTC INITS flag). */
bool      hal_rtc_is_set(void);

/* Field-range check only (no per-month day-count / leap-year logic —
 * the RTC itself normalises). Exposed for callers that want to validate
 * before calling hal_rtc_set(). */
bool      hal_rtc_datetime_valid(const rtc_datetime_t *dt);

#endif /* HAL_RTC_H */
