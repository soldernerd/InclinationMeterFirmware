#ifndef DRV_ENCODER_H
#define DRV_ENCODER_H

#include <stdint.h>
#include "drv_common.h"

typedef enum {
    ENCODER_1 = 0,
    ENCODER_2 = 1,
} EncoderInstance;

/* Full 4x quadrature decode (every edge of both A and B evaluated — see
 * drv_encoder.c). drv_encoder_get_count() returns the raw signed
 * transition count, not a mechanical-detent count: this board's encoder
 * part isn't confirmed to be 4 transitions per detent, so translating
 * "raw transitions" into "clicks the user felt" is left to a higher
 * layer (a future UI/input service) once that's known. */
DrvStatus drv_encoder_init(EncoderInstance instance);
int32_t   drv_encoder_get_count(EncoderInstance instance);
void      drv_encoder_reset(EncoderInstance instance);

#endif /* DRV_ENCODER_H */
