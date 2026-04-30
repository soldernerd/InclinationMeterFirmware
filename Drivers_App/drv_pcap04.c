/* WP1 stub — implemented in WPx */
#include "drv_pcap04.h"

DrvStatus drv_pcap04_init(Pcap04Instance instance) { (void)instance; return DRV_OK; }
DrvStatus drv_pcap04_read(Pcap04Instance instance, int32_t *cap_af)
{
    (void)instance;
    if (cap_af) *cap_af = 0;
    return DRV_OK;
}
bool drv_pcap04_is_ok(Pcap04Instance instance) { (void)instance; return false; }
