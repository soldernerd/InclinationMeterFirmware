#ifndef DRV_RN4871_H
#define DRV_RN4871_H

#include <stdint.h>
#include <stdbool.h>
#include "drv_common.h"

DrvStatus drv_rn4871_init(void);
bool      drv_rn4871_is_connected(void);
DrvStatus drv_rn4871_send(const uint8_t *data, uint16_t len);

#endif /* DRV_RN4871_H */
