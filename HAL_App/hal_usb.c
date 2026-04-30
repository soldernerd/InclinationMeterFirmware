/* WP1 stub — implemented in WPx */
#include "hal_usb.h"

void      hal_usb_init(void)                                       {}
bool      hal_usb_is_connected(void)                               { return false; }
DrvStatus hal_usb_send(const uint8_t *data, uint16_t len)
{
    (void)data; (void)len;
    return DRV_OK;
}
