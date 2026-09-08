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
 * transition count, not a mechanical-detent count; App/app_ui.c's
 * consume_detents() divides by the EEPROM-backed
 * g_device_settings.encoder_counts_per_detent to get "clicks the user
 * felt" (this board's part isn't confirmed to be 4 transitions/detent). */
DrvStatus drv_encoder_init(EncoderInstance instance);
int32_t   drv_encoder_get_count(EncoderInstance instance);

#endif /* DRV_ENCODER_H */
