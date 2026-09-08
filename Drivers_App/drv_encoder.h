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
/* Pure 4x-quadrature step (testable — tests/test_transfer.c). prev_ab /
 * now_ab are 2-bit (A<<1)|B states; returns +1/-1 on the 8 valid
 * single-step Gray-code transitions, 0 on a repeat or an invalid
 * double-step. +1 = the CW sign convention (a naming choice — swap the
 * table's signs if bring-up shows a spin backwards). */
static inline int8_t drv_encoder_quad_step(uint8_t prev_ab, uint8_t now_ab)
{
    static const int8_t tbl[16] = {
         0, -1,  1,  0,
         1,  0,  0, -1,
        -1,  0,  0,  1,
         0,  1, -1,  0,
    };
    return tbl[(((prev_ab & 3U) << 2) | (now_ab & 3U))];
}

DrvStatus drv_encoder_init(EncoderInstance instance);
int32_t   drv_encoder_get_count(EncoderInstance instance);

#endif /* DRV_ENCODER_H */
