#include "hal_rtc.h"
#include "rtc.h"                 /* hrtc, MX_RTC_Init already run in main() */
#include "stm32g0xx_hal.h"

/* See hal_rtc.h. All HAL calls use RTC_FORMAT_BIN so we never touch BCD. */

extern RTC_HandleTypeDef hrtc;

/* Sakamoto's algorithm — 0 = Sunday .. 6 = Saturday, valid for years
 * 1753+. Returned as 1..7 Mon..Sun to match RTC_WEEKDAY_MONDAY..SUNDAY. */
static uint8_t weekday_of(uint16_t y, uint8_t m, uint8_t d)
{
    static const int8_t t[] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    uint16_t yy = y;
    if (m < 3) { yy -= 1; }
    int dow = (yy + yy / 4 - yy / 100 + yy / 400 + t[m - 1] + d) % 7;  /* 0=Sun */
    return (uint8_t)(dow == 0 ? 7 : dow);                              /* -> 1..7 Mon..Sun */
}

void hal_rtc_init(void)
{
    /* Nothing to do — MX_RTC_Init() configured the peripheral. Kept as a
     * call site so the boot sequence in main() reads uniformly and there
     * is somewhere to hang future backup-domain setup. */
}

bool hal_rtc_datetime_valid(const rtc_datetime_t *dt)
{
    return dt != 0
        && dt->year   >= 2000U && dt->year   <= 2099U
        && dt->month  >= 1U    && dt->month  <= 12U
        && dt->day    >= 1U    && dt->day    <= 31U
        && dt->hour   <= 23U
        && dt->minute <= 59U
        && dt->second <= 59U;
}

void hal_rtc_get(rtc_datetime_t *out)
{
    if (out == 0) {
        return;
    }
    RTC_TimeTypeDef tm;
    RTC_DateTypeDef dt;
    /* TR must be read before DR: reading TR locks the shadow registers,
     * reading DR unlocks them again. HAL enforces the pair internally
     * but only if called in this order. */
    HAL_RTC_GetTime(&hrtc, &tm, RTC_FORMAT_BIN);
    HAL_RTC_GetDate(&hrtc, &dt, RTC_FORMAT_BIN);

    out->year    = (uint16_t)(2000U + dt.Year);
    out->month   = dt.Month;
    out->day     = dt.Date;
    out->weekday = dt.WeekDay;
    out->hour    = tm.Hours;
    out->minute  = tm.Minutes;
    out->second  = tm.Seconds;
}

DrvStatus hal_rtc_set(const rtc_datetime_t *in)
{
    if (!hal_rtc_datetime_valid(in)) {
        return DRV_ERR_INVALID;
    }

    RTC_DateTypeDef dt = {0};
    dt.Year    = (uint8_t)(in->year - 2000U);
    dt.Month   = in->month;                     /* RTC_MONTH_* == 1..12 in BIN */
    dt.Date    = in->day;
    dt.WeekDay = weekday_of(in->year, in->month, in->day);

    RTC_TimeTypeDef tm = {0};
    tm.Hours          = in->hour;
    tm.Minutes        = in->minute;
    tm.Seconds        = in->second;
    tm.DayLightSaving = RTC_DAYLIGHTSAVING_NONE;
    tm.StoreOperation = RTC_STOREOPERATION_RESET;

    if (HAL_RTC_SetTime(&hrtc, &tm, RTC_FORMAT_BIN) != HAL_OK) {
        return DRV_ERR_COMM;
    }
    if (HAL_RTC_SetDate(&hrtc, &dt, RTC_FORMAT_BIN) != HAL_OK) {
        return DRV_ERR_COMM;
    }
    return DRV_OK;
}

bool hal_rtc_is_set(void)
{
    return __HAL_RTC_GET_FLAG(&hrtc, RTC_FLAG_INITS) != 0U;
}
