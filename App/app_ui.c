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

static int32_t s_enc1_base;
static int32_t s_enc2_base;

/* ---- per-setting metadata ----
 * Single shared table (label/unit/step/min/max) — App/app_display.c
 * pulls label/unit from here via app_ui_setting_meta() instead of
 * keeping its own parallel switch statements. */

static const UiSettingMeta s_setting_meta[UI_SETTING_COUNT] = {
    [UI_SETTING_DISPLAY_RATE]     = { "Display rate",     "ms",   10,    50,    500 },
    [UI_SETTING_BATTERY_CRITICAL] = { "Battery critical",  "mV",   10,  3000,   3700 },
    [UI_SETTING_STREAM_INTERVAL]  = { "Stream interval",  "ms",   50,   100,   2000 },
    [UI_SETTING_SETTLING_TIMEOUT] = { "Settling timeout", "ms", 1000,  5000,  60000 },
};

const UiSettingMeta *app_ui_setting_meta(UiSettingIndex i)
{
    if ((unsigned)i >= UI_SETTING_COUNT) {
        i = UI_SETTING_DISPLAY_RATE;
    }
    return &s_setting_meta[i];
}

int32_t app_ui_setting_read(UiSettingIndex i)
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
 * detent between calls (the remainder stays banked in *base).
 *
 * Divisor comes from g_device_settings.encoder_counts_per_detent
 * (EEPROM-backed, DEFAULT_ENCODER_COUNTS_PER_DETENT seed in config.h) —
 * NOT confirmed against this board's actual encoder part (2026-08-17
 * user note: "not even sure about the edges per dent"), which is
 * exactly why it's a settings field and not a flash constant: it can be
 * corrected after hardware bring-up without a reflash. Not re-validated
 * here on every call: Services/svc_storage.c's svc_storage_init() is the
 * single gate this value passes through (runs before app_ui_init(), and
 * nothing else in this codebase writes this field), so it's already
 * guaranteed nonzero by the time this runs. */
static int16_t consume_detents(int32_t current_count, int32_t *base)
{
    int32_t per_detent = (int32_t)g_device_settings.encoder_counts_per_detent;
    int32_t delta = current_count - *base;
    int32_t detents = delta / per_detent;
    *base += detents * per_detent;
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
    g_ui_state.edit_value = app_ui_setting_read((UiSettingIndex)g_ui_state.settings_cursor);
    g_ui_state.settings_editing = true;
    g_ui_state.redraw_needed = true;
}

static void commit_edit(void)
{
    UiSettingIndex idx = (UiSettingIndex)g_ui_state.settings_cursor;
    setting_write(idx, g_ui_state.edit_value);
    if (svc_storage_save_settings(&g_device_settings) == DRV_OK) {
        /* Reload task periods so e.g. display rate change takes effect
         * now. This is the preferred entry point for a runtime settings
         * change (app_scheduler_init() is boot-only and also now safe
         * to call again, but only degrades to this same behavior — see
         * its comment). */
        app_scheduler_reload_periods();
        g_ui_state.settings_editing = false;
        /* Deliberately NOT clearing settings_save_failed here — queueing
         * the write successfully doesn't mean it actually completed
         * (Services/svc_storage.c writes 5 EEPROM pages sequentially,
         * asynchronously, over many ticks; a later page can still fail
         * after this returns DRV_OK). svc_storage_update() is the only
         * place that clears the flag, at the point the whole multi-page
         * save genuinely finishes — see system_state.h's comment. */
    } else {
        /* Save failed — stay in edit mode rather than silently pretending
         * it saved, and escalate into system_state (CLAUDE.md 7.6: no
         * silent failures — "must be logged via DBG_PRINT at minimum and
         * escalated to system state"). No debug-UART logging exists in
         * this codebase yet (WP1.5 was never wired up) to also log it. */
        g_ui_state.settings_editing = true;
        g_system_state.settings_save_failed = true;
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

    /* WP3: the LEFT encoder's rotation currently just mirrors the RIGHT
     * one (was "reserved for future scroll") so both knobs can be
     * exercised. All the rotation branches below act on e1_steps. */
    e1_steps = (int16_t)(e1_steps + e2_steps);

    /* LEFT encoder push — universal "back / cancel" */
    if (e2_press) {
        drv_buzzer_beep(BUZZER_TONE_CLICK, 40);
        if (g_ui_state.settings_editing) {
            cancel_edit();
        } else {
            switch_screen(g_ui_state.previous_screen);
        }
    }

    /* RIGHT encoder rotate (or LEFT, mirrored above) — context-dependent */
    if (g_ui_state.current_screen == UI_SCREEN_SETTINGS && g_ui_state.settings_editing) {
        /* Editing a value */
        if (e1_steps != 0) {
            const UiSettingMeta *m = app_ui_setting_meta((UiSettingIndex)g_ui_state.settings_cursor);
            int32_t delta = (int32_t)e1_steps * m->step;
            g_ui_state.edit_value = clamp(g_ui_state.edit_value + delta, m->min_v, m->max_v);
            drv_buzzer_beep(BUZZER_TONE_CLICK, 20);
            g_ui_state.redraw_needed = true;
        }
        if (e1_press) {
            commit_edit();
            drv_buzzer_beep(BUZZER_TONE_CLICK, 40);
        }
    } else if (g_ui_state.current_screen == UI_SCREEN_SETTINGS) {
        /* Settings screen, not editing — rotate moves cursor */
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
        /* RIGHT encoder push on LIVE/STATUS does nothing in WP3 */
    }
}
