#ifndef DRV_SHARP_LCD_H
#define DRV_SHARP_LCD_H

#include <stdint.h>
#include <stdbool.h>
#include "drv_common.h"

#define LCD_WIDTH       400
#define LCD_HEIGHT      240
#define LCD_STRIDE      (LCD_WIDTH / 8)     /* 50 bytes per row */

void      drv_sharp_lcd_init(void);
void      drv_sharp_lcd_clear_buffer(void);
void      drv_sharp_lcd_clear_display(void);
void      drv_sharp_lcd_set_pixel(uint16_t x, uint16_t y, bool on);
void      drv_sharp_lcd_write_row(uint16_t row, const uint8_t *src);
void      drv_sharp_lcd_mark_dirty(uint16_t row);
void      drv_sharp_lcd_mark_all_dirty(void);

/* Bring-up self-test: paint a fixed black/white band pattern straight into
 * the framebuffer (no u8g2). Caller still drives the flush. Pattern: solid
 * 8px bar along the top edge + a solid 8px bar down the left edge (an "L"
 * corner marker), then 16px-tall alternating horizontal bands. */
void      drv_sharp_lcd_selftest_fill(void);
DrvStatus drv_sharp_lcd_flush(void);
DrvStatus drv_sharp_lcd_flush_full(void);
bool      drv_sharp_lcd_is_busy(void);

/* Pumps the post-DMA CS-release state machine — must be called regularly
 * from normal (non-ISR) context, e.g. once per scheduler tick. Finishing
 * a flush (draining the SPI shift register, holding CS, deasserting it)
 * happens here instead of inside the DMA-complete ISR; see the comment on
 * on_dma_complete() in drv_sharp_lcd.c for why that used to be able to
 * hang the whole MCU. */
void      drv_sharp_lcd_update(void);

#endif /* DRV_SHARP_LCD_H */
