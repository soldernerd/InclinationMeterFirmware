#include "drv_lm35.h"
#include "hal_adc.h"
#include "system_state.h"

/* LM35 datasheet: linear 10 mV/°C, VOUT = 0 mV at 0°C -- no offset or
 * segmented fit needed (unlike Drivers_App/drv_tmp236.c's TMP236, which
 * needs a two-segment piecewise-linear correction above 100°C). Fixed-
 * point (centidegrees, 0.01°C/LSB, matching this codebase's temp_cdeg
 * convention):
 *
 *   temp_cdeg = v_mv * 100 / scale_mv_per_c
 *
 * g_device_settings.lm35_scale_mv_per_c is EEPROM-backed (nominal 10, see
 * config.h's DEFAULT_LM35_SCALE_MV_PER_C, zero-guarded in
 * Services/svc_storage.c) rather than a #define here -- no calibration
 * constant lives only in flash (project rule), matching every other
 * sensor driver. */

void drv_lm35_init(void)
{
    /* hal_adc owns the underlying ADC. Nothing to do here. */
}

DrvStatus drv_lm35_start_read(void)
{
    hal_adc_start();
    return DRV_OK;
}

DrvStatus drv_lm35_get_result(lm35_data_t *out)
{
    if (out == 0) {
        return DRV_ERR_INVALID;
    }
    /* Gate on the ADC's persistent data-freshness flag, not the transient
     * "ready" flag -- same reasoning as Drivers_App/drv_tmp236.c's
     * drv_tmp236_get_result(). */
    adc_results_t r = hal_adc_get_results();
    if (!r.valid) {
        return DRV_ERR_NOT_READY;
    }

    uint32_t v_mv = hal_adc_raw_to_mv(r.temp_ext_raw, r.vrefint_raw);
    int32_t  temp_cdeg = (int32_t)v_mv * 100 / (int32_t)g_device_settings.lm35_scale_mv_per_c;

    /* LM35 datasheet range: -55°C to +150°C (-5500 to 15000 centidegrees)
     * -- reject anything outside a margin of that as a fault (disconnected
     * sensor, short to a rail, ESD-diode clamping), not a real reading.
     * Unlike Drivers_App/drv_tmp236.c's TMP236 (~19.5-19.7 mV/°C, whose
     * segment-2 formula tops out around 14822 cdeg even at ADC rail-to-
     * rail input), LM35's lower 10 mV/°C slope means a fault condition
     * (e.g. ~3300 mV at VDDA rail-to-rail) computes to 33000 cdeg here --
     * past INT16_MAX (32767). Checking before narrowing avoids silently
     * wrapping that into a plausible-looking but wrong (and wrong-sign)
     * int16_t value while still reporting DRV_OK (CLAUDE.md 7.6). */
    if (temp_cdeg < -6000 || temp_cdeg > 16000) {
        return DRV_ERR_INVALID;
    }

    out->temp_cdeg = (int16_t)temp_cdeg;
    out->status    = DRV_OK;
    return DRV_OK;
}
