/* WP1 stub — implemented in WPx */
#include "hal_adc.h"

void      hal_adc_init(void)                                            {}
DrvStatus hal_adc_read_channel(uint8_t channel, uint16_t *raw)
{
    (void)channel;
    if (raw) *raw = 0;
    return DRV_OK;
}
