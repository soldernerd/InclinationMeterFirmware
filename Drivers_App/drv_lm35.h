#ifndef DRV_LM35_H
#define DRV_LM35_H

#include <stdint.h>
#include "drv_common.h"

DrvStatus drv_lm35_init(void);
DrvStatus drv_lm35_read_cdeg(int16_t *temp_cdeg);

#endif /* DRV_LM35_H */
