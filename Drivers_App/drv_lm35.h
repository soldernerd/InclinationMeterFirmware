#ifndef DRV_LM35_H
#define DRV_LM35_H

#include <stdint.h>
#include "drv_common.h"

typedef struct {
    int16_t   temp_cdeg;    /* 0.01 °C / LSB */
    DrvStatus status;
} lm35_data_t;

void      drv_lm35_init(void);
DrvStatus drv_lm35_start_read(void);               /* triggers ADC scan via hal_adc */
DrvStatus drv_lm35_get_result(lm35_data_t *out);   /* returns DRV_ERR_NOT_READY until
                                                      * the ADC has produced valid data */

#endif /* DRV_LM35_H */
