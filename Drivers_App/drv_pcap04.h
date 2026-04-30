#ifndef DRV_PCAP04_H
#define DRV_PCAP04_H

#include <stdint.h>
#include <stdbool.h>
#include "drv_common.h"

typedef enum {
    PCAP04_1 = 0,
    PCAP04_2 = 1,
} Pcap04Instance;

DrvStatus drv_pcap04_init(Pcap04Instance instance);
DrvStatus drv_pcap04_read(Pcap04Instance instance, int32_t *cap_af);
bool      drv_pcap04_is_ok(Pcap04Instance instance);

#endif /* DRV_PCAP04_H */
