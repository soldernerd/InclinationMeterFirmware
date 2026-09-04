#include "app_display.h"
#include "app_ui.h"
#include "drv_sharp_lcd.h"
#include "u8g2_hal_callback.h"
#include "app_version.h"
#include "system_state.h"
#include "svc_battery.h"
#include "svc_api.h"
#include "svc_measurement.h"
#include "hal_systick.h"
#include "hal_usb.h"     /* WP4 bring-up: USB diag block on the STATUS screen */
#include "config.h"
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
    MeasurementState meas_state;
    ApiMode  api_mode_usb;
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
    u8g2_SetFont(&s_u8g2, u8g2_font_7x13_tr);
    u8g2_DrawUTF8(&s_u8g2, 4, 12, "InclinationMeter");

    /* Right-anchored badges + battery percent */
    char buf[16];
    snprintf(buf, sizeof buf, "BAT:%u%%", (unsigned)g_system_state.battery_soc_pct);
    u8g2_uint_t bat_w = u8g2_GetUTF8Width(&s_u8g2, buf);
    u8g2_uint_t x = (u8g2_uint_t)(LCD_WIDTH - 4 - bat_w);
    u8g2_DrawUTF8(&s_u8g2, x, 12, buf);

    /* Status badges to the left of BAT — measurement state has highest
     * priority, then transport mode, then charging, then USB. */
    if (svc_measurement_get_state() != MEAS_STATE_IDLE) {
        const char *t = "[SGL]";
        u8g2_uint_t w = u8g2_GetUTF8Width(&s_u8g2, t);
        x = (u8g2_uint_t)(x - 2 - w);
        u8g2_DrawUTF8(&s_u8g2, x, 12, t);
    }
    ApiMode m_usb = svc_api_get_mode(API_TRANSPORT_USB);
    if (m_usb == API_MODE_RAW_STREAM) {
        const char *t = "[RAW]";
        u8g2_uint_t w = u8g2_GetUTF8Width(&s_u8g2, t);
        x = (u8g2_uint_t)(x - 2 - w);
        u8g2_DrawUTF8(&s_u8g2, x, 12, t);
    } else if (m_usb == API_MODE_STREAM) {
        const char *t = "[STR]";
        u8g2_uint_t w = u8g2_GetUTF8Width(&s_u8g2, t);
        x = (u8g2_uint_t)(x - 2 - w);
        u8g2_DrawUTF8(&s_u8g2, x, 12, t);
    }
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

/* MEASURING overlay — drawn on top of whichever screen is active when
 * svc_measurement_get_state() != IDLE. Replaces the screen body, keeps
 * the top bar visible. */
static void draw_measuring_overlay(void)
{
    u8g2_SetFont(&s_u8g2, u8g2_font_ncenB14_tr);
    const char *title = "Measuring...";
    u8g2_uint_t w = u8g2_GetUTF8Width(&s_u8g2, title);
    u8g2_DrawUTF8(&s_u8g2, (u8g2_uint_t)((LCD_WIDTH - w) / 2), 60, title);

    /* Progress bar — 320 px wide, 14 px tall, centred */
    uint8_t pct = svc_measurement_get_progress_pct();
    u8g2_uint_t bar_w = 320;
    u8g2_uint_t bar_x = (u8g2_uint_t)((LCD_WIDTH - bar_w) / 2);
    u8g2_uint_t bar_y = 100;
    u8g2_DrawFrame(&s_u8g2, bar_x, bar_y, bar_w, 14);
    u8g2_DrawBox(&s_u8g2, (u8g2_uint_t)(bar_x + 2), (u8g2_uint_t)(bar_y + 2),
                 (u8g2_uint_t)(((uint32_t)(bar_w - 4) * pct) / 100U), 10);

    char line[40];
    snprintf(line, sizeof line, "%u%%", (unsigned)pct);
    u8g2_SetFont(&s_u8g2, u8g2_font_6x10_tr);
    w = u8g2_GetUTF8Width(&s_u8g2, line);
    u8g2_DrawUTF8(&s_u8g2, (u8g2_uint_t)((LCD_WIDTH - w) / 2), 134, line);

    snprintf(line, sizeof line, "Samples: %u / %u",
             (unsigned)svc_measurement_get_packet()->sample_count,
             (unsigned)SETTLING_BUFFER_SIZE);
    w = u8g2_GetUTF8Width(&s_u8g2, line);
    u8g2_DrawUTF8(&s_u8g2, (u8g2_uint_t)((LCD_WIDTH - w) / 2), 154, line);

    const char *hint = "[ENC2 push] Cancel";
    w = u8g2_GetUTF8Width(&s_u8g2, hint);
    u8g2_DrawUTF8(&s_u8g2, (u8g2_uint_t)((LCD_WIDTH - w) / 2), 200, hint);
}

/* ---- screen indicator (bottom) ---- */

static void draw_screen_indicator(UiScreen current)
{
    static const char *labels[UI_SCREEN_COUNT] = { "LIVE", "STATUS", "SETTINGS" };
    u8g2_SetFont(&s_u8g2, u8g2_font_7x13_tr);

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

    u8g2_SetFont(&s_u8g2, u8g2_font_7x13_tr);
    u8g2_DrawUTF8(&s_u8g2, 8, 38, "Temperature");

    char line[32];
    snprintf(line, sizeof line, "%s C", temp_str);
    u8g2_SetFont(&s_u8g2, u8g2_font_logisoso24_tr);
    u8g2_DrawUTF8(&s_u8g2, 8, 76, line);

    u8g2_SetFont(&s_u8g2, u8g2_font_7x13_tr);
    u8g2_DrawUTF8(&s_u8g2, 8, 110, "Battery");

    char volt_str[16];
    format_volts(volt_str, sizeof volt_str, svc_battery_get_vbat_mv());
    snprintf(line, sizeof line, "%u%%   %s",
             (unsigned)g_system_state.battery_soc_pct, volt_str);
    u8g2_SetFont(&s_u8g2, u8g2_font_logisoso24_tr);
    u8g2_DrawUTF8(&s_u8g2, 8, 148, line);

    u8g2_SetFont(&s_u8g2, u8g2_font_7x13_tr);
    u8g2_DrawUTF8(&s_u8g2, 8, 200, "Sensors: offline");
}

/* ---- STATUS screen ---- */

static void draw_status_screen(void)
{
    char line[40];
    char up_str[16];
    format_uptime(up_str, sizeof up_str, hal_systick_get_ms());

    u8g2_SetFont(&s_u8g2, u8g2_font_7x13_tr);
    int y = 38;
    snprintf(line, sizeof line, "Firmware:  v%s", FW_VERSION_STRING);
    u8g2_DrawUTF8(&s_u8g2, 8, (u8g2_uint_t)y, line);  y += 18;
    {
        uint8_t st = g_system_state.eeprom_selftest;
        if (st == 0U) {
            snprintf(line, sizeof line, "EEPROM RW: OK");
        } else if (st == 255U) {
            snprintf(line, sizeof line, "EEPROM RW: (not run)");
        } else {
            snprintf(line, sizeof line, "EEPROM RW: FAIL %u", (unsigned)st);
        }
    }
    u8g2_DrawUTF8(&s_u8g2, 8, (u8g2_uint_t)y, line);                           y += 18;
    u8g2_DrawUTF8(&s_u8g2, 8, (u8g2_uint_t)y,
                  g_system_state.settings_save_failed ? "Settings:  SAVE FAILED"
                                                      : "Settings:  loaded");   y += 18;
    u8g2_DrawUTF8(&s_u8g2, 8, (u8g2_uint_t)y,
                  g_system_state.ble_connected ? "BLE:       connected"
                                               : "BLE:       offline");       y += 18;
    u8g2_DrawUTF8(&s_u8g2, 8, (u8g2_uint_t)y,
                  g_system_state.usb_connected ? "USB:       connected"
                                               : "USB:       offline");       y += 18;
    snprintf(line, sizeof line, "Uptime:    %s", up_str);
    u8g2_DrawUTF8(&s_u8g2, 8, (u8g2_uint_t)y, line);  y += 18;

    /* ---- WP4 USB bring-up diagnostics (temporary) ----
     * Kept to exactly 4 lines (146/164/182/200) so nothing collides with
     * draw_screen_indicator()'s text at y=232 — a 5th line here previously
     * did, and was invisible/garbled under the indicator.
     *
     * a=VBUS-gated attach latch, v=raw PA2, st=USBD dev_state (1 DEFAULT /
     *   2 ADDRESSED / 3 CONFIGURED / 4 SUSPENDED), pu=D+ pull-up
     *   (BCDR.DPPU), fnr=USB frame number (moves only while host SOF is
     *   received), tog=USBD_Start()/Stop() calls since boot (climbing fast
     *   while still plugged in would mean VBUS_SENSE/PA2 is bouncing).
     * irq=USB IRQ entries since boot; gO/gB=SYSCFG->IT_LINE_SR[8] bit-2
     *   (USB) gate open/blocked counts — HAL_PCD_IRQHandler's very first
     *   check, before touching ISTR at all. A single snapshot of that
     *   register can't tell "blocked on 95% of entries, this one happened
     *   to be open" from "reliably open"; these two running totals can. If
     *   gB dominates gO, this gate is eating every event (not even
     *   clearing ISTR.RESET) before our PCD_*Callback hooks ever run —
     *   explains hw (raw ISTR.RESET seen) climbing while rst/setup never do.
     * rst/setup/susp=PCD_Reset/SetupStage/Suspend callback entries; hw=raw
     *   ISTR.RESET seen at IRQ entry, BEFORE HAL_PCD_IRQHandler runs at all
     *   — compare against rst. */
    {
        HalUsbDebug u;
        hal_usb_get_debug(&u);
        snprintf(line, sizeof line, "a%u v%u st%u pu%u fnr%u tog%lu",
                 (unsigned)u.attached, (unsigned)u.vbus_pin,
                 (unsigned)u.dev_state, (unsigned)u.dppu,
                 (unsigned)u.fnr, (unsigned long)u.attach_toggles);
        u8g2_DrawUTF8(&s_u8g2, 8, (u8g2_uint_t)y, line);  y += 18;
        snprintf(line, sizeof line, "irq%lu gO%lu gB%lu",
                 (unsigned long)u.irq_count,
                 (unsigned long)u.it_line_gate_open_count,
                 (unsigned long)u.it_line_gate_blocked_count);
        u8g2_DrawUTF8(&s_u8g2, 8, (u8g2_uint_t)y, line);  y += 18;
        snprintf(line, sizeof line, "rst%lu(hw%lu) setup%lu susp%lu",
                 (unsigned long)u.reset_count, (unsigned long)u.reset_flag_seen,
                 (unsigned long)u.setup_count, (unsigned long)u.suspend_count);
        u8g2_DrawUTF8(&s_u8g2, 8, (u8g2_uint_t)y, line);  y += 18;
        /* crsE=sticky OR of every CRS->ISR read since boot (bit0 SYNCOKF) —
         * catches a transient lock a single live read could miss; not just
         * "never seen set in whatever reading happened to run".
         * hsi48on/rdy = RCC->CR HSI48ON/HSI48RDY — confirms the oscillator
         * itself is enabled and stable. usbsel = RCC->CCIPR2 USBSEL field,
         * expected 0 (HSI48); a nonzero value would mean the USB peripheral
         * is clocked from something else entirely.
         * usv=PWR->CR2 VDDUSB-supply-valid. If 0, the analog transceiver
         * runs under-supplied: coarse SE0/reset detection can still work,
         * but real differential signal levels (needed to decode any actual
         * packet) may not — matching resets-work/data-never-decodes. */
        snprintf(line, sizeof line, "crsE%u hsi48%u%u sel%u usv%u",
                 (unsigned)(u.crs_isr_ever & 1U), (unsigned)u.hsi48_on,
                 (unsigned)u.hsi48_rdy, (unsigned)u.usb_clk_sel,
                 (unsigned)u.vddusb_valid);
        u8g2_DrawUTF8(&s_u8g2, 8, (u8g2_uint_t)y, line);
    }
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
    u8g2_SetFont(&s_u8g2, u8g2_font_7x13_tr);
    u8g2_DrawUTF8(&s_u8g2, 8, 36, "Settings");

    int y = 56;
    for (uint8_t i = 0; i < UI_SETTING_COUNT; ++i) {
        char line[64];
        const char *cursor = (i == g_ui_state.settings_cursor) ? ">" : " ";
        const UiSettingMeta *m = app_ui_setting_meta((UiSettingIndex)i);
        bool is_selected_and_editing = (i == g_ui_state.settings_cursor) && g_ui_state.settings_editing;
        if (m->step != 0) {
            snprintf(line, sizeof line, "%s %-18s %ld %s",
                     cursor,
                     m->label,
                     (long)setting_value_for_display((UiSettingIndex)i),
                     m->unit);
        } else {
            /* Action row (step == 0, e.g. "Reboot to DFU") — no numeric
             * value to show; a confirm prompt replaces it once the row is
             * "entered" (the first RIGHT press), same highlight box as an
             * ordinary edit. */
            snprintf(line, sizeof line, "%s %-18s%s",
                     cursor, m->label,
                     is_selected_and_editing ? "  RIGHT again to confirm" : "");
        }
        if (is_selected_and_editing) {
            /* Highlight: invert background of this row */
            u8g2_SetDrawColor(&s_u8g2, 1);
            u8g2_DrawBox(&s_u8g2, 4, (u8g2_uint_t)(y - 14), LCD_WIDTH - 8, 19);
            u8g2_SetDrawColor(&s_u8g2, 0);
            u8g2_DrawUTF8(&s_u8g2, 8, (u8g2_uint_t)y, line);
            u8g2_SetDrawColor(&s_u8g2, 1);
        } else {
            u8g2_DrawUTF8(&s_u8g2, 8, (u8g2_uint_t)y, line);
        }
        y += 18;
    }

    u8g2_DrawUTF8(&s_u8g2, 8, 200, "LEFT: screen   RIGHT: item/value   RIGHT press: enter");

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
        || s_last.meas_state       != svc_measurement_get_state()
        || s_last.api_mode_usb     != svc_api_get_mode(API_TRANSPORT_USB)
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
    s_last.meas_state       = svc_measurement_get_state();
    s_last.api_mode_usb     = svc_api_get_mode(API_TRANSPORT_USB);
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
    } else if (svc_measurement_get_state() != MEAS_STATE_IDLE) {
        /* Measurement in progress — overlay replaces the screen body
         * but the top bar still gives status context. */
        draw_top_bar();
        draw_measuring_overlay();
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
