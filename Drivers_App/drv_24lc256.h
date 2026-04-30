#ifndef DRV_24LC256_H
#define DRV_24LC256_H

#include <stdint.h>
#include "drv_common.h"

DrvStatus drv_24lc256_init(void);
DrvStatus drv_24lc256_read(uint16_t addr, uint8_t *data, uint16_t len);
DrvStatus drv_24lc256_write(uint16_t addr, const uint8_t *data, uint16_t len);

#endif /* DRV_24LC256_H */
