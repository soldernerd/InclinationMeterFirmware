#ifndef DRV_TMP236_H
#define DRV_TMP236_H

#include <stdint.h>
#include "drv_common.h"

/* TI TMP236 on-board temperature sensor — TEMP_SENSE (PB12/ADC1_IN16),
 * powered from the 5V rail. See pin_config.h's TEMP_SENSE_PIN comment. */

typedef struct {
    int16_t   temp_cdeg;    /* 0.01 °C / LSB */
    DrvStatus status;
} tmp236_data_t;

void      drv_tmp236_init(void);
DrvStatus drv_tmp236_start_read(void);                /* triggers ADC scan via hal_adc */
DrvStatus drv_tmp236_get_result(tmp236_data_t *out);  /* DRV_ERR_NOT_READY until the
                                                         * ADC has produced valid data */

#endif /* DRV_TMP236_H */
