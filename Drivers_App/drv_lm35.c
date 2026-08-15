#include "drv_lm35.h"
#include "hal_adc.h"
#include "pin_config.h"

void drv_lm35_init(void)
{
    /* hal_adc owns the underlying ADC. Nothing to do here in WP2. */
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
    /* Gate on the ADC's data-freshness flag, not hal_adc_is_ready(): that
     * flag is cleared the instant any caller (e.g. the faster-running ADC
     * scheduler task) restarts a new scan, which starves slower consumers
     * of ever seeing it true. .valid persists once the first scan lands
     * and only clears on an actual ADC error — see hal_adc.c. */
    const adc_results_t *r = hal_adc_get_results();
    if (!r->valid) {
        return DRV_ERR_NOT_READY;
    }
    /* Conversion per pin_config.h: temp_cdeg = adc_raw × 5 (0.01 °C / LSB).
     * NOTE: this is the LM35 formula. REV B's on-board sensor (PB12) is a
     * TI TMP236, not an LM35 — unverified against the TMP236 datasheet,
     * see pin_config.h's TEMP_SENSE_PIN comment. Readings from this
     * function are not yet trustworthy for absolute temperature. */
    out->temp_cdeg = (int16_t)(r->temp_raw * (uint32_t)LM35_SCALE);
    out->status    = DRV_OK;
    return DRV_OK;
}
