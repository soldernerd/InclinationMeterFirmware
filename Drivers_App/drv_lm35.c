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

DrvStatus drv_lm35_get_result(Lm35Data *out)
{
    if (out == 0) {
        return DRV_ERR_INVALID;
    }
    if (!hal_adc_is_ready()) {
        return DRV_ERR_NOT_READY;
    }
    /* Conversion per pin_config.h: temp_cdeg = adc_raw × 5 (0.01 °C / LSB).
     * NOTE: this is the LM35 formula. REV B's on-board sensor (PB12) is a
     * TI TMP236, not an LM35 — unverified against the TMP236 datasheet,
     * see pin_config.h's TEMP_SENSE_PIN comment. Readings from this
     * function are not yet trustworthy for absolute temperature. */
    uint32_t raw = hal_adc_read_temp_raw();
    out->temp_cdeg = (int16_t)(raw * (uint32_t)LM35_SCALE);
    out->status    = DRV_OK;
    return DRV_OK;
}
