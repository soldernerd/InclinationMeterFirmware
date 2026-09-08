#ifndef DRV_TMP236_H
#define DRV_TMP236_H

#include <stdint.h>
#include "drv_common.h"

/* TI TMP236 on-board temperature sensor — TEMP_SENSE (PB12/ADC1_IN16),
 * powered from the 5V rail. See pin_config.h's TEMP_SENSE_PIN comment. */

typedef struct {
    int16_t   temp_cdeg;    /* 0.01 °C / LSB */
    DrvStatus status;
} tmp236_data_t;

/* --- transfer function (pure, testable — tests/test_transfer.c) ---
 * TI TMP236 datasheet (SBOS857E Rev E) §7.3 / Table 2: a two-segment
 * linear fit is more accurate above 100 °C than one line across the range.
 *   TA = (VOUT - VOFFS) / TC + TINFL          (datasheet Eq. 2)
 *
 *   TA range (°C)   VRANGE (mV)   TINFL (°C)   TC (mV/°C)   VOFFS (mV)
 *   -40 to 100       <= 2350       0            19.5         400
 *    100 to 125      >  2350       100          19.7         2350
 *
 * Fixed-point (centidegrees, TC as an integer ratio):
 *   seg 1: cdeg = (v_mv -  400) * 200 / 39  + 0
 *   seg 2: cdeg = (v_mv - 2350) * 1000 / 197 + 10000
 * Continuous at the 2350 mV boundary (seg 1 == 10000 there == seg 2's
 * TINFL*100). The eight constants live in EEPROM (g_device_settings.tmp236_*,
 * config.h DEFAULT_TMP236_*) — no calibration constant lives only in flash.
 * drv_tmp236_get_result() passes them in via this struct. */
typedef struct {
    uint16_t seg_boundary_mv;
    uint16_t seg1_voffs_mv, seg1_num, seg1_den;
    uint16_t seg2_voffs_mv, seg2_num, seg2_den, seg2_tinfl_cdeg;
} tmp236_cal_t;

static inline int32_t drv_tmp236_mv_to_cdeg(uint32_t v_mv, const tmp236_cal_t *c)
{
    if (v_mv <= c->seg_boundary_mv) {
        return ((int32_t)v_mv - (int32_t)c->seg1_voffs_mv)
               * (int32_t)c->seg1_num / (int32_t)c->seg1_den;
    }
    return ((int32_t)v_mv - (int32_t)c->seg2_voffs_mv)
           * (int32_t)c->seg2_num / (int32_t)c->seg2_den
           + (int32_t)c->seg2_tinfl_cdeg;
}

void      drv_tmp236_init(void);
DrvStatus drv_tmp236_start_read(void);                /* triggers ADC scan via hal_adc */
DrvStatus drv_tmp236_get_result(tmp236_data_t *out);  /* DRV_ERR_NOT_READY until the
                                                         * ADC has produced valid data */

#endif /* DRV_TMP236_H */
