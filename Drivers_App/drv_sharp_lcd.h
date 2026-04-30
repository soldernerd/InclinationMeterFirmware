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
DrvStatus drv_sharp_lcd_flush(void);
DrvStatus drv_sharp_lcd_flush_full(void);
bool      drv_sharp_lcd_is_busy(void);

#endif /* DRV_SHARP_LCD_H */
