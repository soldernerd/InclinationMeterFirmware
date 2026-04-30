#ifndef U8G2_HAL_CALLBACK_H
#define U8G2_HAL_CALLBACK_H

#include "u8g2.h"

uint8_t u8g2_hal_callback(u8x8_t *u8x8, uint8_t msg, uint8_t arg_int, void *arg_ptr);
void    u8g2_hal_init(u8g2_t *u8g2);

#endif /* U8G2_HAL_CALLBACK_H */
