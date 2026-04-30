#ifndef HAL_ADC_H
#define HAL_ADC_H

#include <stdint.h>
#include "drv_common.h"

void      hal_adc_init(void);
DrvStatus hal_adc_read_channel(uint8_t channel, uint16_t *raw);

#endif /* HAL_ADC_H */
