#include "app_display.h"
#include "app_ui.h"
#include "drv_sharp_lcd.h"
#include "u8g2_hal_callback.h"
#include "app_version.h"
#include "system_state.h"
#include "svc_battery.h"
#include "hal_systick.h"
#include "u8g2.h"
#include <stdio.h>
#include <string.h>

static u8g2_t s_u8g2;

/* Track the slow-changing fields we display so we can skip flushes when
 * nothing visible has changed. */
typedef struct {
    int16_t  temperature_cdeg;
    uint16_t battery_mv;
    uint8_t  battery_soc_pct;
    bool     battery_charging;
    bool     usb_connected;
    bool     battery_critical;
    UiScreen screen;
    uint8_t  settings_cursor;
    bool     settings_editing;
    int32_t  edit_value;
    uint32_t uptime_s;      /* only checked while UI_SCREEN_STATUS is
                              * shown — its "Uptime: HH:MM:SS" line is
                              * the only thing that changes purely from
                              * time passing, with nothing else in this
                              * struct tracking it otherwise. */
} DisplaySnapshot;

static DisplaySnapshot s_last = {0};
static bool            s_have_last = false;

/* ---- format helpers ---- */

static void format_temp(char *buf, size_t bufsz, int16_t cdeg)
{
    int sign = cdeg < 0 ? -1 : 1;
    int32_t a = (int32_t)cdeg * sign;
    snprintf(buf, bufsz, "%s%ld.%02ld", sign < 0 ? "-" : "",
             (long)(a / 100), (long)(a % 100));
}

static void format_volts(char *buf, size_t bufsz, uint16_t mv)
{
    snprintf(buf, bufsz, "%u.%02uV",
             (unsigned)(mv / 1000U), (unsigned)((mv % 1000U) / 10U));
}

static void format_uptime(char *buf, size_t bufsz, uint32_t ms)
{
    uint32_t s  = ms / 1000U;
    uint32_t mn = (s / 60U) % 60U;
    uint32_t hh = (s / 3600U);
    uint32_t ss = s % 60U;
    snprintf(buf, bufsz, "%02lu:%02lu:%02lu",
             (unsigned long)hh, (unsigned long)mn, (unsigned long)ss);
}

/* ---- top bar ---- */

static void draw_top_bar(void)
{
    u8g2_SetFont(&s_u8g2, u8g2_font_6x10_tr);
    u8g2_DrawUTF8(&s_u8g2, 4, 12, "InclinationMeter");

    /* Right-anchored badges + battery percent */
    char buf[16];
    snprintf(buf, sizeof buf, "BAT:%u%%", (unsigned)g_system_state.battery_soc_pct);
    u8g2_uint_t bat_w = u8g2_GetUTF8Width(&s_u8g2, buf);
    u8g2_uint_t x = (u8g2_uint_t)(LCD_WIDTH - 4 - bat_w);
    u8g2_DrawUTF8(&s_u8g2, x, 12, buf);

    /* Status badges to the left of BAT */
    if (g_system_state.battery_charging) {
        const char *t = "[CHG]";
        u8g2_uint_t w = u8g2_GetUTF8Width(&s_u8g2, t);
        x = (u8g2_uint_t)(x - 2 - w);
        u8g2_DrawUTF8(&s_u8g2, x, 12, t);
    }
    if (g_system_state.usb_connected) {
        const char *t = "[USB]";
        u8g2_uint_t w = u8g2_GetUTF8Width(&s_u8g2, t);
        x = (u8g2_uint_t)(x - 2 - w);
        u8g2_DrawUTF8(&s_u8g2, x, 12, t);
    }

    u8g2_DrawHLine(&s_u8g2, 0, 18, LCD_WIDTH);
}

/* ---- screen indicator (bottom) ---- */

static void draw_screen_indicator(UiScreen current)
{
    static const char *labels[UI_SCREEN_COUNT] = { "LIVE", "STATUS", "SETTINGS" };
    u8g2_SetFont(&s_u8g2, u8g2_font_6x10_tr);

    /* Lay out evenly across the width */
    u8g2_uint_t y = 232;
    u8g2_uint_t third = (u8g2_uint_t)(LCD_WIDTH / UI_SCREEN_COUNT);
    for (uint8_t i = 0; i < UI_SCREEN_COUNT; ++i) {
        u8g2_uint_t w = u8g2_GetUTF8Width(&s_u8g2, labels[i]);
        u8g2_uint_t cx = (u8g2_uint_t)(i * third + (third - w) / 2U);
        if (i == current) {
            char framed[16];
            snprintf(framed, sizeof framed, "<%s>", labels[i]);
            w = u8g2_GetUTF8Width(&s_u8g2, framed);
            cx = (u8g2_uint_t)(i * third + (third - w) / 2U);
            u8g2_DrawUTF8(&s_u8g2, cx, y, framed);
        } else {
            u8g2_DrawUTF8(&s_u8g2, cx, y, labels[i]);
        }
    }
}

/* ---- LIVE screen ---- */

static void draw_live_screen(void)
{
    char temp_str[16];
    format_temp(temp_str, sizeof temp_str, g_system_state.temperature_cdeg);

    u8g2_SetFont(&s_u8g2, u8g2_font_6x10_tr);
    u8g2_DrawUTF8(&s_u8g2, 8, 38, "Temperature");

    char line[32];
    snprintf(line, sizeof line, "%s C", temp_str);
    u8g2_SetFont(&s_u8g2, u8g2_font_logisoso24_tr);
    u8g2_DrawUTF8(&s_u8g2, 8, 76, line);

    u8g2_SetFont(&s_u8g2, u8g2_font_6x10_tr);
    u8g2_DrawUTF8(&s_u8g2, 8, 110, "Battery");

    char volt_str[16];
    format_volts(volt_str, sizeof volt_str, svc_battery_get_vbat_mv());
    snprintf(line, sizeof line, "%u%%   %s",
             (unsigned)g_system_state.battery_soc_pct, volt_str);
    u8g2_SetFont(&s_u8g2, u8g2_font_logisoso24_tr);
    u8g2_DrawUTF8(&s_u8g2, 8, 148, line);

    u8g2_SetFont(&s_u8g2, u8g2_font_6x10_tr);
    u8g2_DrawUTF8(&s_u8g2, 8, 200, "Sensors: offline");
}

/* ---- STATUS screen ---- */

static void draw_status_screen(void)
{
    char line[40];
    char up_str[16];
    format_uptime(up_str, sizeof up_str, hal_systick_get_ms());

    u8g2_SetFont(&s_u8g2, u8g2_font_6x10_tr);
    int y = 38;
    snprintf(line, sizeof line, "Firmware:  v%s", FW_VERSION_STRING);
    u8g2_DrawUTF8(&s_u8g2, 8, (u8g2_uint_t)y, line);  y += 16;
    u8g2_DrawUTF8(&s_u8g2, 8, (u8g2_uint_t)y, "EEPROM:    OK");                y += 16;
    u8g2_DrawUTF8(&s_u8g2, 8, (u8g2_uint_t)y, "Settings:  loaded");            y += 16;
    u8g2_DrawUTF8(&s_u8g2, 8, (u8g2_uint_t)y,
                  g_system_state.ble_connected ? "BLE:       connected"
                                               : "BLE:       offline");       y += 16;
    u8g2_DrawUTF8(&s_u8g2, 8, (u8g2_uint_t)y,
                  g_system_state.usb_connected ? "USB:       connected"
                                               : "USB:       offline");       y += 16;
    snprintf(line, sizeof line, "Uptime:    %s", up_str);
    u8g2_DrawUTF8(&s_u8g2, 8, (u8g2_uint_t)y, line);
}

/* ---- SETTINGS screen ---- */

static int32_t setting_value_for_display(UiSettingIndex i)
{
    /* If editing the row at cursor, show the working edit_value;
     * otherwise show the live setting via app_ui.c's own read function —
     * not a re-implemented switch, so the two can't desync. */
    if (g_ui_state.settings_editing
        && (UiSettingIndex)g_ui_state.settings_cursor == i) {
        return g_ui_state.edit_value;
    }
    return app_ui_setting_read(i);
}

static void draw_settings_screen(void)
{
    u8g2_SetFont(&s_u8g2, u8g2_font_6x10_tr);
    u8g2_DrawUTF8(&s_u8g2, 8, 36, "Settings");

    int y = 56;
    for (uint8_t i = 0; i < UI_SETTING_COUNT; ++i) {
        char line[64];
        const char *cursor = (i == g_ui_state.settings_cursor) ? ">" : " ";
        const UiSettingMeta *m = app_ui_setting_meta((UiSettingIndex)i);
        snprintf(line, sizeof line, "%s %-18s %ld %s",
                 cursor,
                 m->label,
                 (long)setting_value_for_display((UiSettingIndex)i),
                 m->unit);
        if (i == g_ui_state.settings_cursor && g_ui_state.settings_editing) {
            /* Highlight: invert background of this row */
            u8g2_SetDrawColor(&s_u8g2, 1);
            u8g2_DrawBox(&s_u8g2, 4, (u8g2_uint_t)(y - 12), LCD_WIDTH - 8, 16);
            u8g2_SetDrawColor(&s_u8g2, 0);
            u8g2_DrawUTF8(&s_u8g2, 8, (u8g2_uint_t)y, line);
            u8g2_SetDrawColor(&s_u8g2, 1);
        } else {
            u8g2_DrawUTF8(&s_u8g2, 8, (u8g2_uint_t)y, line);
        }
        y += 16;
    }

    u8g2_DrawUTF8(&s_u8g2, 8, 200, "[ENC1] select/edit  [ENC2] back");

    /* Makes g_system_state.settings_save_failed actually visible — a
     * prior review pass added the flag (escalating a failed EEPROM
     * write per CLAUDE.md's "No Silent Failures" rule) but nothing
     * rendered it, so a real save failure was recorded but invisible
     * to the user. */
    if (g_system_state.settings_save_failed) {
        u8g2_DrawUTF8(&s_u8g2, 8, 216, "! SAVE FAILED - retry !");
    }
}

/* ---- low-battery overlay (carried over from WP2) ---- */

static void draw_low_battery_screen(uint16_t vbat_mv)
{
    char volt_str[16];
    format_volts(volt_str, sizeof volt_str, vbat_mv);

    u8g2_SetFont(&s_u8g2, u8g2_font_ncenB14_tr);
    const char *line1 = "! LOW BATTERY !";
    u8g2_uint_t w = u8g2_GetUTF8Width(&s_u8g2, line1);
    u8g2_DrawUTF8(&s_u8g2, (u8g2_uint_t)((LCD_WIDTH - w) / 2), 100, line1);

    u8g2_SetFont(&s_u8g2, u8g2_font_ncenB10_tr);
    const char *line2 = g_system_state.battery_critical ? "Shutting down..."
                                                        : "Connect charger";
    w = u8g2_GetUTF8Width(&s_u8g2, line2);
    u8g2_DrawUTF8(&s_u8g2, (u8g2_uint_t)((LCD_WIDTH - w) / 2), 130, line2);

    w = u8g2_GetUTF8Width(&s_u8g2, volt_str);
    u8g2_DrawUTF8(&s_u8g2, (u8g2_uint_t)((LCD_WIDTH - w) / 2), 160, volt_str);
}

/* ---- change detection ---- */

static bool snapshot_changed(void)
{
    if (!s_have_last) return true;
    return s_last.temperature_cdeg != g_system_state.temperature_cdeg
        || s_last.battery_mv       != svc_battery_get_vbat_mv()
        || s_last.battery_soc_pct  != g_system_state.battery_soc_pct
        || s_last.battery_charging != g_system_state.battery_charging
        || s_last.usb_connected    != g_system_state.usb_connected
        || s_last.battery_critical != g_system_state.battery_critical
        || s_last.screen           != g_ui_state.current_screen
        || s_last.settings_cursor  != g_ui_state.settings_cursor
        || s_last.settings_editing != g_ui_state.settings_editing
        || s_last.edit_value       != g_ui_state.edit_value
        || (g_ui_state.current_screen == UI_SCREEN_STATUS
            && s_last.uptime_s != hal_systick_get_ms() / 1000U);
}

static void snapshot_capture(void)
{
    s_last.temperature_cdeg = g_system_state.temperature_cdeg;
    s_last.battery_mv       = svc_battery_get_vbat_mv();
    s_last.battery_soc_pct  = g_system_state.battery_soc_pct;
    s_last.battery_charging = g_system_state.battery_charging;
    s_last.usb_connected    = g_system_state.usb_connected;
    s_last.battery_critical = g_system_state.battery_critical;
    s_last.screen           = g_ui_state.current_screen;
    s_last.settings_cursor  = g_ui_state.settings_cursor;
    s_last.settings_editing = g_ui_state.settings_editing;
    s_last.edit_value       = g_ui_state.edit_value;
    s_last.uptime_s         = hal_systick_get_ms() / 1000U;
    s_have_last = true;
}

/* ---- public API ---- */

void app_display_init(void)
{
    drv_sharp_lcd_init();
    u8g2_hal_init(&s_u8g2);
    u8g2_InitDisplay(&s_u8g2);
    u8g2_SetPowerSave(&s_u8g2, 0);
    g_ui_state.redraw_needed = true;
    s_have_last = false;
}

void app_display_update(void)
{
    if (drv_sharp_lcd_is_busy()) {
        return;
    }
    if (!g_ui_state.redraw_needed && !snapshot_changed()) {
        return;
    }

    u8g2_ClearBuffer(&s_u8g2);
    u8g2_SetDrawColor(&s_u8g2, 1);

    if (g_system_state.battery_low) {
        draw_low_battery_screen(svc_battery_get_vbat_mv());
    } else {
        draw_top_bar();
        switch (g_ui_state.current_screen) {
            case UI_SCREEN_LIVE:     draw_live_screen();     break;
            case UI_SCREEN_STATUS:   draw_status_screen();   break;
            case UI_SCREEN_SETTINGS: draw_settings_screen(); break;
            default:                                          break;
        }
        draw_screen_indicator(g_ui_state.current_screen);
    }

    u8g2_SendBuffer(&s_u8g2);
    (void)drv_sharp_lcd_flush_full();

    g_ui_state.redraw_needed = false;
    snapshot_capture();
}
