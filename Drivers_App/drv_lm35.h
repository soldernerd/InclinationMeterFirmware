#ifndef DRV_LM35_H
#define DRV_LM35_H

#include <stdint.h>
#include "drv_common.h"

/* LM35 external temperature sensor -- TEMP_SENSE_EXT (PB11/ADC1_IN15).
 * Linear 10 mV/°C, 0 mV at 0°C (no offset, unlike Drivers_App/drv_tmp236.c's
 * TMP236, which needs a two-segment piecewise-linear fit above 100°C). See
 * config.h's DEFAULT_LM35_SCALE_MV_PER_C. Shares the same underlying ADC
 * scan as TMP236 -- see hal_adc.h's adc_results_t (CH15/temp_ext_raw). */

typedef struct {
    int16_t   temp_cdeg;    /* 0.01 °C / LSB */
    DrvStatus status;
} lm35_data_t;

/* Transfer function (pure, testable — tests/test_transfer.c). LM35:
 * 10 mV/°C, 0 mV at 0 °C. scale_mv_per_c is EEPROM-backed (nominal 10,
 * g_device_settings.lm35_scale_mv_per_c, config.h DEFAULT_LM35_SCALE_MV_PER_C,
 * zero-guarded in svc_storage.c). */
static inline int32_t drv_lm35_mv_to_cdeg(uint32_t v_mv, uint16_t scale_mv_per_c)
{
    return (int32_t)v_mv * 100 / (int32_t)scale_mv_per_c;
}

void      drv_lm35_init(void);
DrvStatus drv_lm35_get_result(lm35_data_t *out);    /* DRV_ERR_NOT_READY until the
                                                       * ADC has produced valid data */

#endif /* DRV_LM35_H */
