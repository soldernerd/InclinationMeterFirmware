#ifndef DRV_SCL3300_H
#define DRV_SCL3300_H

#include <stdint.h>
#include <stdbool.h>
#include "drv_common.h"

DrvStatus drv_scl3300_init(void);
DrvStatus drv_scl3300_read(int16_t *x_cdeg, int16_t *y_cdeg, int16_t *z_cdeg);
bool      drv_scl3300_is_ok(void);

#endif /* DRV_SCL3300_H */
