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
DrvStatus drv_sharp_lcd_flush_full(void);
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
