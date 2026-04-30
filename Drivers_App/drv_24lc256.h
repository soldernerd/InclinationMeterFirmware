#ifndef DRV_24LC256_H
#define DRV_24LC256_H

#include <stdint.h>
#include <stdbool.h>
#include "drv_common.h"

#define EEPROM_I2C_ADDR     0x50U
#define EEPROM_PAGE_SIZE    64U
#define EEPROM_TOTAL_BYTES  32768U

void      drv_24lc256_init(void);

/* Non-blocking read via DMA */
DrvStatus drv_24lc256_start_read(uint16_t addr, uint8_t *buf, uint16_t len);
bool      drv_24lc256_read_complete(void);

/* Non-blocking page write — caller must respect EEPROM_PAGE_SIZE alignment
 * and not cross page boundaries. svc_storage handles multi-page chunking. */
DrvStatus drv_24lc256_start_write_page(uint16_t addr,
                                       const uint8_t *buf,
                                       uint16_t len);
bool      drv_24lc256_is_busy(void);    /* true while DMA active or write cycle polling */

/* Drive the internal state machine — call from scheduler every tick.
 * Polls EEPROM ACK to detect end of internal write cycle. */
void      drv_24lc256_update(void);

#endif /* DRV_24LC256_H */
