#include "app_display.h"
#include "drv_sharp_lcd.h"
#include "u8g2_hal_callback.h"
#include "app_version.h"
#include "u8g2.h"

/* Bring-up switch. 1 = bypass u8g2 entirely and just push a hard-coded
 * black/white band pattern to the panel, to sanity-check the
 * SPI / CS-timing / panel-power / VCOM path. Set back to 0 for the real UI. */
#define LCD_SELFTEST 1

#if !LCD_SELFTEST
static u8g2_t s_u8g2;

static void draw_hello_world(void)
{
    u8g2_ClearBuffer(&s_u8g2);

    u8g2_SetFont(&s_u8g2, u8g2_font_ncenB14_tr);
    const char *msg = "Hello World";
    u8g2_uint_t w = u8g2_GetUTF8Width(&s_u8g2, msg);
    u8g2_DrawUTF8(&s_u8g2, (u8g2_uint_t)((400 - w) / 2), 120, msg);

    u8g2_SetFont(&s_u8g2, u8g2_font_6x10_tr);
    const char *banner = "InclinationMeterFirmware WP1 v" FW_VERSION_STRING;
    u8g2_uint_t bw = u8g2_GetUTF8Width(&s_u8g2, banner);
    u8g2_DrawUTF8(&s_u8g2, (u8g2_uint_t)((400 - bw) / 2), 232, banner);

    u8g2_SendBuffer(&s_u8g2);
}
#endif /* !LCD_SELFTEST */

void app_display_init(void)
{
    drv_sharp_lcd_init();

#if !LCD_SELFTEST
    u8g2_hal_init(&s_u8g2);
    u8g2_InitDisplay(&s_u8g2);
    u8g2_SetPowerSave(&s_u8g2, 0);

    draw_hello_world();
    (void)drv_sharp_lcd_flush_full();
#endif
}

void app_display_update(void)
{
    if (drv_sharp_lcd_is_busy()) {
        return;
    }

#if LCD_SELFTEST
    /* Repaint + reflush the fixed pattern whenever the panel is idle.
     * Idempotent; VCOM keeps toggling via TIM6 regardless. */
    drv_sharp_lcd_selftest_fill();
    (void)drv_sharp_lcd_flush_full();
#else
    draw_hello_world();
    (void)drv_sharp_lcd_flush_full();
#endif
}
