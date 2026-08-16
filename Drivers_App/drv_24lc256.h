#ifndef DRV_24LC256_H
#define DRV_24LC256_H

#include <stdint.h>
#include <stdbool.h>
#include "drv_common.h"

/* Verified against the actual datasheet (Microchip 24AA256/24LC256/24FC256,
 * local copy: .../InclinationMeter/_archive/Datasheets/24LC256.pdf), not
 * assumed from memory:
 *   - Control byte = 1010 + A2 A1 A0 (Section 5.0). A0/A1/A2 are all tied
 *     to GND on this board, so the client address is 1010000b = 0x50U —
 *     confirms EEPROM_I2C_ADDR below was already correct.
 *   - WP pin is tied to GND (VSS) on this board -> write protection is
 *     permanently disabled, writes are always possible (Section 6.3). Not
 *     wired to any MCU pin, no firmware action needed or possible.
 *   - TWC (self-timed write cycle time) = 5 ms max (Section timing specs) —
 *     confirms drv_24lc256.c's OP_WRITE_CYCLE_POLL comment and its 50 ms
 *     give-up timeout (10x margin).
 *   - Page write buffer = 64 bytes, matches EEPROM_PAGE_SIZE below. A page
 *     write that crosses a physical page boundary wraps within the page
 *     instead of proceeding into the next one (Section 6.2) — this is why
 *     drv_24lc256_start_write_page() rejects addr/len combinations that
 *     would cross a boundary rather than trying to handle it. */
#define EEPROM_I2C_ADDR     0x50U
#define EEPROM_PAGE_SIZE    64U
#define EEPROM_TOTAL_BYTES  32768U

void      drv_24lc256_init(void);

/* Non-blocking read via DMA — the address-write phase is DMA-driven too,
 * not blocking. */
DrvStatus drv_24lc256_start_read(uint16_t addr, uint8_t *buf, uint16_t len);
bool      drv_24lc256_read_complete(void);   /* true = succeeded; call only after !is_busy() */

/* Non-blocking page write — caller must respect EEPROM_PAGE_SIZE alignment
 * and not cross page boundaries. svc_storage handles multi-page chunking. */
DrvStatus drv_24lc256_start_write_page(uint16_t addr,
                                       const uint8_t *buf,
                                       uint16_t len);
bool      drv_24lc256_write_complete(void);  /* true = succeeded; call only after !is_busy() */
bool      drv_24lc256_is_busy(void);    /* true while DMA active or write cycle polling */

/* Drive the internal state machine — call from scheduler every tick.
 * Polls EEPROM ACK to detect end of internal write cycle. */
void      drv_24lc256_update(void);

/* Abandons whatever operation is in flight: aborts the underlying I2C
 * transfer and forces the state machine back to idle. Use when a caller
 * (e.g. svc_storage.c's blocking helpers) has given up waiting on its own
 * timeout and can no longer guarantee the buffer it passed to
 * drv_24lc256_start_read()/start_write_page() stays valid — without this,
 * a late DMA completion could still write into memory the caller has
 * since reused (e.g. a stack buffer that's gone out of scope). */
void      drv_24lc256_abort(void);

#endif /* DRV_24LC256_H */
