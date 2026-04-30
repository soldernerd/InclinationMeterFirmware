/* WP1 stub — implemented in WPx */
#include "drv_rn4871.h"

DrvStatus drv_rn4871_init(void)                               { return DRV_OK; }
bool      drv_rn4871_is_connected(void)                       { return false; }
DrvStatus drv_rn4871_send(const uint8_t *data, uint16_t len)  { (void)data; (void)len; return DRV_OK; }
