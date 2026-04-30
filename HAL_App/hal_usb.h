#ifndef HAL_USB_H
#define HAL_USB_H

#include <stdint.h>
#include <stdbool.h>
#include "drv_common.h"

void      hal_usb_init(void);
bool      hal_usb_is_connected(void);
DrvStatus hal_usb_send(const uint8_t *data, uint16_t len);

#endif /* HAL_USB_H */
