/* WP1 stub — implemented in WPx */
#include "drv_scl3300.h"

DrvStatus drv_scl3300_init(void) { return DRV_OK; }
DrvStatus drv_scl3300_read(int16_t *x_cdeg, int16_t *y_cdeg, int16_t *z_cdeg)
{
    if (x_cdeg) *x_cdeg = 0;
    if (y_cdeg) *y_cdeg = 0;
    if (z_cdeg) *z_cdeg = 0;
    return DRV_OK;
}
bool drv_scl3300_is_ok(void) { return false; }
