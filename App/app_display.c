#include "app_display.h"
#include "drv_sharp_lcd.h"
#include "u8g2_hal_callback.h"
#include "app_version.h"
#include "system_state.h"
#include "svc_battery.h"
#include "u8g2.h"
#include <stdio.h>

static u8g2_t s_u8g2;

/* ---- helpers ---- */

static void format_temp(char *buf, size_t bufsz, int16_t cdeg)
{
    /* cdeg is 0.01°C; print whole and hundredths */
    int sign = cdeg < 0 ? -1 : 1;
    int32_t abs_cdeg = (int32_t)cdeg * sign;
    int32_t whole = abs_cdeg / 100;
    int32_t frac  = abs_cdeg % 100;
    snprintf(buf, bufsz, "%s%ld.%02ld", sign < 0 ? "-" : "", (long)whole, (long)frac);
}

static void format_volts(char *buf, size_t bufsz, uint16_t mv)
{
    /* 4180 mV → "4.18V" */
    uint16_t whole = mv / 1000U;
    uint16_t frac  = (mv % 1000U) / 10U;     /* hundredths */
    snprintf(buf, bufsz, "%u.%02uV", (unsigned)whole, (unsigned)frac);
}

/* ---- screens ---- */

static void draw_low_battery_screen(uint16_t vbat_mv)
{
    char volt_str[16];
    format_volts(volt_str, sizeof volt_str, vbat_mv);

    u8g2_ClearBuffer(&s_u8g2);

    u8g2_SetFont(&s_u8g2, u8g2_font_ncenB14_tr);
    const char *line1 = "! LOW BATTERY !";
    u8g2_uint_t w = u8g2_GetUTF8Width(&s_u8g2, line1);
    u8g2_DrawUTF8(&s_u8g2, (u8g2_uint_t)((LCD_WIDTH - w) / 2), 100, line1);

    u8g2_SetFont(&s_u8g2, u8g2_font_ncenB10_tr);
    const char *line2 = "Shutting down...";
    w = u8g2_GetUTF8Width(&s_u8g2, line2);
    u8g2_DrawUTF8(&s_u8g2, (u8g2_uint_t)((LCD_WIDTH - w) / 2), 130, line2);

    u8g2_SetFont(&s_u8g2, u8g2_font_ncenB10_tr);
    w = u8g2_GetUTF8Width(&s_u8g2, volt_str);
    u8g2_DrawUTF8(&s_u8g2, (u8g2_uint_t)((LCD_WIDTH - w) / 2), 160, volt_str);

    u8g2_SendBuffer(&s_u8g2);
}

static void draw_status_screen(void)
{
    char line[40];

    u8g2_ClearBuffer(&s_u8g2);

    /* Top bar */
    u8g2_SetFont(&s_u8g2, u8g2_font_6x10_tr);
    u8g2_DrawUTF8(&s_u8g2, 4, 12, "InclinationMeter");
    snprintf(line, sizeof line, "v%s", FW_VERSION_STRING);
    u8g2_uint_t w = u8g2_GetUTF8Width(&s_u8g2, line);
    u8g2_DrawUTF8(&s_u8g2, (u8g2_uint_t)(LCD_WIDTH - 4 - w), 12, line);
    u8g2_DrawHLine(&s_u8g2, 0, 18, LCD_WIDTH);

    /* Temperature */
    u8g2_SetFont(&s_u8g2, u8g2_font_6x10_tr);
    u8g2_DrawUTF8(&s_u8g2, 8, 40, "Temperature");

    char temp_str[16];
    format_temp(temp_str, sizeof temp_str, g_system_state.temperature_cdeg);
    snprintf(line, sizeof line, "%s C", temp_str);
    u8g2_SetFont(&s_u8g2, u8g2_font_logisoso24_tr);
    u8g2_DrawUTF8(&s_u8g2, 8, 78, line);

    /* Battery */
    u8g2_SetFont(&s_u8g2, u8g2_font_6x10_tr);
    u8g2_DrawUTF8(&s_u8g2, 8, 110, "Battery");

    char volt_str[16];
    format_volts(volt_str, sizeof volt_str, svc_battery_get_vbat_mv());
    snprintf(line, sizeof line, "%u%%   %s",
             (unsigned)g_system_state.battery_soc_pct, volt_str);
    u8g2_SetFont(&s_u8g2, u8g2_font_logisoso24_tr);
    u8g2_DrawUTF8(&s_u8g2, 8, 148, line);

    /* Status badges */
    u8g2_SetFont(&s_u8g2, u8g2_font_6x10_tr);
    u8g2_uint_t bx = 8;
    if (g_system_state.battery_charging) {
        u8g2_DrawFrame(&s_u8g2, bx, 168, 70, 14);
        u8g2_DrawUTF8(&s_u8g2, (u8g2_uint_t)(bx + 8), 178, "CHARGING");
        bx = (u8g2_uint_t)(bx + 80);
    }
    if (g_system_state.usb_connected) {
        u8g2_DrawFrame(&s_u8g2, bx, 168, 36, 14);
        u8g2_DrawUTF8(&s_u8g2, (u8g2_uint_t)(bx + 8), 178, "USB");
    }

    /* Sensors placeholder */
    u8g2_DrawUTF8(&s_u8g2, 8, 210, "Sensors: offline");

    /* Bottom version banner */
    u8g2_SetFont(&s_u8g2, u8g2_font_5x7_tr);
    snprintf(line, sizeof line, "InclinationMeterFirmware WP2 v%s", FW_VERSION_STRING);
    w = u8g2_GetUTF8Width(&s_u8g2, line);
    u8g2_DrawUTF8(&s_u8g2, (u8g2_uint_t)((LCD_WIDTH - w) / 2), 234, line);

    u8g2_SendBuffer(&s_u8g2);
}

/* ---- public API ---- */

void app_display_init(void)
{
    drv_sharp_lcd_init();

    u8g2_hal_init(&s_u8g2);
    u8g2_InitDisplay(&s_u8g2);
    u8g2_SetPowerSave(&s_u8g2, 0);

    draw_status_screen();
    (void)drv_sharp_lcd_flush_full();
}

void app_display_update(void)
{
    if (drv_sharp_lcd_is_busy()) {
        return;
    }
    if (g_system_state.battery_critical) {
        draw_low_battery_screen(svc_battery_get_vbat_mv());
    } else {
        draw_status_screen();
    }
    (void)drv_sharp_lcd_flush_full();
}
