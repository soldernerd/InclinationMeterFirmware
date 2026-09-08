#include "drv_tmp236.h"
#include "hal_adc.h"
#include "system_state.h"

/* The two-segment transfer function is drv_tmp236_mv_to_cdeg() in the
 * header (pure — see tests/test_transfer.c). This file just wires the ADC
 * scan and the EEPROM-backed calibration constants into it. */

void drv_tmp236_init(void)
{
    /* hal_adc owns the underlying ADC. Nothing to do here. */
}

DrvStatus drv_tmp236_start_read(void)
{
    hal_adc_start();
    return DRV_OK;
}

DrvStatus drv_tmp236_get_result(tmp236_data_t *out)
{
    if (out == 0) {
        return DRV_ERR_INVALID;
    }
    /* Gate on the ADC's persistent data-freshness flag, not the transient
     * "ready" flag — see hal_adc.c / App/app_scheduler.c for why: "ready"
     * gets cleared the instant any faster-running consumer restarts a scan,
     * starving slower consumers of ever observing it true. */
    adc_results_t r = hal_adc_get_results();
    if (!r.valid) {
        return DRV_ERR_NOT_READY;
    }

    uint32_t v_mv = hal_adc_raw_to_mv(r.temp_raw, r.vrefint_raw);
    const tmp236_cal_t cal = {
        .seg_boundary_mv = g_device_settings.tmp236_seg_boundary_mv,
        .seg1_voffs_mv   = g_device_settings.tmp236_seg1_voffs_mv,
        .seg1_num        = g_device_settings.tmp236_seg1_num,
        .seg1_den        = g_device_settings.tmp236_seg1_den,
        .seg2_voffs_mv   = g_device_settings.tmp236_seg2_voffs_mv,
        .seg2_num        = g_device_settings.tmp236_seg2_num,
        .seg2_den        = g_device_settings.tmp236_seg2_den,
        .seg2_tinfl_cdeg = g_device_settings.tmp236_seg2_tinfl_cdeg,
    };

    out->temp_cdeg = (int16_t)drv_tmp236_mv_to_cdeg(v_mv, &cal);
    out->status    = DRV_OK;
    return DRV_OK;
}
