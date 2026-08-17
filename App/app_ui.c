#include "app_ui.h"
#include "drv_buzzer.h"
#include "svc_storage.h"
#include "app_scheduler.h"
#include "system_state.h"

UiState g_ui_state = {
    .current_screen   = UI_SCREEN_LIVE,
    .previous_screen  = UI_SCREEN_LIVE,
    .settings_cursor  = 0,
    .settings_editing = false,
    .redraw_needed    = true,
    .edit_value       = 0,
};

/* Raw quadrature transitions per mechanical detent — NOT confirmed
 * against this board's actual encoder part (2026-08-17 user note: "not
 * even sure about the edges per dent"). 4 is the standard assumption for
 * a detented incremental encoder; revisit once real hardware is
 * available — if a click needs two nudges (or half a nudge triggers a
 * step), this is the one constant to change. */
#define ENCODER_COUNTS_PER_DETENT  4

static int32_t s_enc1_base;
static int32_t s_enc2_base;

/* ---- per-setting metadata ---- */

typedef struct {
    int32_t  step;
    int32_t  min_v;
    int32_t  max_v;
} SettingRange;

static const SettingRange s_ranges[UI_SETTING_COUNT] = {
    [UI_SETTING_DISPLAY_RATE]     = {   10,    50,    500 },
    [UI_SETTING_BATTERY_CRITICAL] = {   10,  3000,   3700 },
    [UI_SETTING_STREAM_INTERVAL]  = {   50,   100,   2000 },
    [UI_SETTING_SETTLING_TIMEOUT] = { 1000,  5000,  60000 },
};

static int32_t setting_read(UiSettingIndex i)
{
    switch (i) {
        case UI_SETTING_DISPLAY_RATE:     return (int32_t)g_device_settings.task_display_ms;
        case UI_SETTING_BATTERY_CRITICAL: return (int32_t)g_device_settings.battery_critical_mv;
        case UI_SETTING_STREAM_INTERVAL:  return (int32_t)g_device_settings.stream_interval_ms;
        case UI_SETTING_SETTLING_TIMEOUT: return (int32_t)g_device_settings.settling_timeout_ms;
        default:                          return 0;
    }
}

static void setting_write(UiSettingIndex i, int32_t v)
{
    switch (i) {
        case UI_SETTING_DISPLAY_RATE:
            g_device_settings.task_display_ms = (uint16_t)v;     break;
        case UI_SETTING_BATTERY_CRITICAL:
            g_device_settings.battery_critical_mv = (uint16_t)v; break;
        case UI_SETTING_STREAM_INTERVAL:
            g_device_settings.stream_interval_ms = (uint16_t)v;  break;
        case UI_SETTING_SETTLING_TIMEOUT:
            g_device_settings.settling_timeout_ms = (uint32_t)v; break;
        default:                                                 break;
    }
}

static int32_t clamp(int32_t v, int32_t lo, int32_t hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

/* Converts a continuously-accumulating raw quadrature count into whole
 * mechanical detents since the last call, without losing a partial
 * detent between calls (the remainder stays banked in *base). */
static int16_t consume_detents(int32_t current_count, int32_t *base)
{
    int32_t delta = current_count - *base;
    int32_t detents = delta / ENCODER_COUNTS_PER_DETENT;
    *base += detents * ENCODER_COUNTS_PER_DETENT;
    return (int16_t)detents;
}

/* ---- helpers ---- */

static void switch_screen(UiScreen target)
{
    if (target >= UI_SCREEN_COUNT) return;
    g_ui_state.previous_screen = g_ui_state.current_screen;
    g_ui_state.current_screen  = target;
    g_ui_state.settings_editing = false;
    g_ui_state.redraw_needed   = true;
}

static UiScreen next_screen(UiScreen s)
{
    return (UiScreen)(((unsigned)s + 1U) % UI_SCREEN_COUNT);
}

static UiScreen prev_screen(UiScreen s)
{
    return (UiScreen)(((unsigned)s + UI_SCREEN_COUNT - 1U) % UI_SCREEN_COUNT);
}

static void enter_edit_mode(void)
{
    g_ui_state.edit_value = setting_read((UiSettingIndex)g_ui_state.settings_cursor);
    g_ui_state.settings_editing = true;
    g_ui_state.redraw_needed = true;
}

static void commit_edit(void)
{
    UiSettingIndex idx = (UiSettingIndex)g_ui_state.settings_cursor;
    setting_write(idx, g_ui_state.edit_value);
    if (svc_storage_save_settings(&g_device_settings) == DRV_OK) {
        /* Reload task periods so e.g. display rate change takes effect now. */
        app_scheduler_init();
        g_ui_state.settings_editing = false;
    } else {
        /* Save failed — stay in edit mode rather than silently pretending
         * it saved (CLAUDE.md 7.6: no silent failures). No debug-UART
         * logging exists in this codebase yet (WP1.5 was never wired up)
         * to report this any further. */
        g_ui_state.settings_editing = true;
    }
    g_ui_state.redraw_needed = true;
}

static void cancel_edit(void)
{
    g_ui_state.settings_editing = false;
    g_ui_state.redraw_needed = true;
}

/* ---- public API ---- */

void app_ui_init(void)
{
    /* drv_encoder_init()/drv_buzzer_init() already run in main.c, before
     * svc_input_init() — not repeated here. */
    s_enc1_base = 0;
    s_enc2_base = 0;

    g_ui_state.current_screen   = UI_SCREEN_LIVE;
    g_ui_state.previous_screen  = UI_SCREEN_LIVE;
    g_ui_state.settings_cursor  = 0;
    g_ui_state.settings_editing = false;
    g_ui_state.redraw_needed    = true;
}

void app_ui_update(void)
{
    int16_t e1_steps = consume_detents(g_system_state.encoder1_count, &s_enc1_base);
    int16_t e2_steps = consume_detents(g_system_state.encoder2_count, &s_enc2_base);
    bool    e1_press = g_system_state.encoder1_sw_press_event;
    bool    e2_press = g_system_state.encoder2_sw_press_event;
    g_system_state.encoder1_sw_press_event = false;
    g_system_state.encoder2_sw_press_event = false;
    (void)e2_steps;     /* ENC2 rotate reserved for future scroll */

    /* Encoder 2 push — universal "back / cancel" */
    if (e2_press) {
        drv_buzzer_beep(BUZZER_TONE_CLICK, 40);
        if (g_ui_state.settings_editing) {
            cancel_edit();
        } else {
            switch_screen(g_ui_state.previous_screen);
        }
    }

    /* Encoder 1 — context-dependent */
    if (g_ui_state.current_screen == UI_SCREEN_SETTINGS && g_ui_state.settings_editing) {
        /* Editing a value */
        if (e1_steps != 0) {
            const SettingRange *r = &s_ranges[g_ui_state.settings_cursor];
            int32_t delta = (int32_t)e1_steps * r->step;
            g_ui_state.edit_value = clamp(g_ui_state.edit_value + delta, r->min_v, r->max_v);
            drv_buzzer_beep(BUZZER_TONE_CLICK, 20);
            g_ui_state.redraw_needed = true;
        }
        if (e1_press) {
            commit_edit();
            drv_buzzer_beep(BUZZER_TONE_CLICK, 40);
        }
    } else if (g_ui_state.current_screen == UI_SCREEN_SETTINGS) {
        /* Settings screen, not editing — encoder1 moves cursor */
        if (e1_steps > 0) {
            g_ui_state.settings_cursor = (uint8_t)((g_ui_state.settings_cursor + 1U)
                                                   % UI_SETTING_COUNT);
            drv_buzzer_beep(BUZZER_TONE_CLICK, 20);
            g_ui_state.redraw_needed = true;
        } else if (e1_steps < 0) {
            g_ui_state.settings_cursor =
                (uint8_t)((g_ui_state.settings_cursor + UI_SETTING_COUNT - 1U)
                          % UI_SETTING_COUNT);
            drv_buzzer_beep(BUZZER_TONE_CLICK, 20);
            g_ui_state.redraw_needed = true;
        }
        if (e1_press) {
            enter_edit_mode();
            drv_buzzer_beep(BUZZER_TONE_CLICK, 40);
        }
    } else {
        /* LIVE / STATUS — encoder1 cycles between screens */
        if (e1_steps > 0) {
            switch_screen(next_screen(g_ui_state.current_screen));
            drv_buzzer_beep(BUZZER_TONE_CLICK, 20);
        } else if (e1_steps < 0) {
            switch_screen(prev_screen(g_ui_state.current_screen));
            drv_buzzer_beep(BUZZER_TONE_CLICK, 20);
        }
        /* Encoder 1 push on LIVE/STATUS does nothing in WP3 */
    }
}
