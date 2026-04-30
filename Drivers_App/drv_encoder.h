#ifndef DRV_ENCODER_H
#define DRV_ENCODER_H

#include <stdint.h>
#include "drv_common.h"

typedef enum {
    ENCODER_1 = 0,
    ENCODER_2 = 1,
} EncoderInstance;

DrvStatus drv_encoder_init(EncoderInstance instance);
int32_t   drv_encoder_get_count(EncoderInstance instance);
void      drv_encoder_reset(EncoderInstance instance);

#endif /* DRV_ENCODER_H */
