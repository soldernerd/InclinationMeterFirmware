/* WP1 stub — implemented in WPx */
#include "drv_lm35.h"

DrvStatus drv_lm35_init(void)                       { return DRV_OK; }
DrvStatus drv_lm35_read_cdeg(int16_t *temp_cdeg)
{
    if (temp_cdeg) *temp_cdeg = 0;
    return DRV_OK;
}
