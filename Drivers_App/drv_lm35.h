#ifndef DRV_LM35_H
#define DRV_LM35_H

#include <stdint.h>
#include "drv_common.h"

typedef struct {
    int16_t   temp_cdeg;    /* 0.01 °C / LSB */
    DrvStatus status;
} Lm35Data;

void      drv_lm35_init(void);
DrvStatus drv_lm35_start_read(void);            /* triggers ADC scan via hal_adc */
DrvStatus drv_lm35_get_result(Lm35Data *out);   /* call after hal_adc_is_ready() */

#endif /* DRV_LM35_H */
