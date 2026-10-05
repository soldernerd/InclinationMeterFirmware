#ifndef DRV_SHARP_LCD_H
#define DRV_SHARP_LCD_H

#include <stdint.h>
#include <stdbool.h>
#include "drv_common.h"

#define LCD_WIDTH       400
#define LCD_HEIGHT      240
#define LCD_STRIDE      (LCD_WIDTH / 8)     /* 50 bytes per row */

void      drv_sharp_lcd_init(void);
void      drv_sharp_lcd_write_row(uint16_t row, const uint8_t *src);

/* write_row() tracks which rows actually changed. flush_dirty() sends only
 * those rows (merged into a few runs, one CS-framed transaction each) and
 * returns DRV_OK without touching the bus if nothing changed;
 * DRV_ERR_NOT_READY if a previous flush is still on the wire (rows stay
 * dirty). mark_all_dirty() forces the whole panel to be resent;
 * has_dirty() lets the App layer retry a flush that was refused. */
DrvStatus drv_sharp_lcd_flush_dirty(void);
void      drv_sharp_lcd_mark_all_dirty(void);
bool      drv_sharp_lcd_has_dirty(void);
bool      drv_sharp_lcd_is_busy(void);

/* Panel health: false after a flush drain-timeout forced an SPI2 re-init;
 * self-heals on the next clean flush. The App layer mirrors this into
 * g_system_state.display_ok — the driver does not touch system state. */
bool      drv_sharp_lcd_ok(void);

/* Pumps the post-DMA CS-release state machine — must be called regularly
 * from normal (non-ISR) context, e.g. once per scheduler tick. Finishing
 * a flush (draining the SPI shift register, holding CS, deasserting it)
 * happens here instead of inside the DMA-complete ISR; see the comment on
 * on_dma_complete() in drv_sharp_lcd.c for why that used to be able to
 * hang the whole MCU. */
void      drv_sharp_lcd_update(void);

#endif /* DRV_SHARP_LCD_H */
