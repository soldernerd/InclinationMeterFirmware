#include "drv_tmp236.h"
#include "hal_adc.h"
#include "system_state.h"

/* TI TMP236 datasheet (SBOS857E, "TMP235, TMP236" Rev E), Section 7.3 /
 * Table 2 "TMP236 Piecewise Linear Function Summary" — a two-segment
 * linear transfer function gives better accuracy above 100°C than a
 * single line fit across the whole range:
 *
 *   VOUT = (TA - TINFL) x TC + VOFFS        (Eq. 1)
 *   TA   = (VOUT - VOFFS) / TC + TINFL      (Eq. 2, inverse — what we use)
 *
 *   TA range (°C)   VRANGE (mV)   TINFL (°C)   TC (mV/°C)   VOFFS (mV)
 *   -40 to 100       <= 2350       0            19.5         400
 *    100 to 125      >  2350       100          19.7         2350
 *
 * Fixed-point (centidegrees, 0.01 °C/LSB — matches this codebase's
 * temp_cdeg convention), TC expressed as an integer ratio:
 *   segment 1: temp_cdeg = (v_mv -  400) x 200 / 39  + 0
 *   segment 2: temp_cdeg = (v_mv - 2350) x 1000 / 197 + 10000
 * (continuous at the 2350 mV boundary: segment 1 evaluates to exactly
 * 10000 there, matching segment 2's TINFL x 100.)
 *
 * The eight constants above are EEPROM-backed (g_device_settings.tmp236_*)
 * rather than #defines here — no calibration constant lives only in flash
 * (project rule). See config.h's DEFAULT_TMP236_* for the seed values.
 */

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
    const adc_results_t *r = hal_adc_get_results();
    if (!r->valid) {
        return DRV_ERR_NOT_READY;
    }

    uint32_t v_mv = hal_adc_raw_to_mv(r->temp_raw);
    int32_t  temp_cdeg;
    if (v_mv <= g_device_settings.tmp236_seg_boundary_mv) {
        temp_cdeg = ((int32_t)v_mv - g_device_settings.tmp236_seg1_voffs_mv)
                    * g_device_settings.tmp236_seg1_num / g_device_settings.tmp236_seg1_den;
    } else {
        temp_cdeg = ((int32_t)v_mv - g_device_settings.tmp236_seg2_voffs_mv)
                    * g_device_settings.tmp236_seg2_num / g_device_settings.tmp236_seg2_den
                    + g_device_settings.tmp236_seg2_tinfl_cdeg;
    }

    out->temp_cdeg = (int16_t)temp_cdeg;
    out->status    = DRV_OK;
    return DRV_OK;
}
