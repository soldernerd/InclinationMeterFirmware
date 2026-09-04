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

static int32_t s_enc1_pos;
static int32_t s_enc2_pos;

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

/* Beep feedback. A nav click scales with how many detents landed in one
 * UI tick (a fast spin), capped, so a burst still gets an audible beep
 * instead of one short blip. drv_buzzer_beep() itself only ever extends
 * an in-progress beep, never shortens it. */
#define BEEP_NAV_MS      40U
#define BEEP_NAV_MAX_MS  110U
#define BEEP_CONFIRM_MS  70U

static void beep_nav(int16_t steps)
{
    uint32_t n  = (uint32_t)(steps < 0 ? -(int32_t)steps : (int32_t)steps);
    if (n == 0U) n = 1U;
    uint32_t ms = BEEP_NAV_MS * n;
    if (ms > BEEP_NAV_MAX_MS) ms = BEEP_NAV_MAX_MS;
    drv_buzzer_beep(BUZZER_TONE_CLICK, (uint16_t)ms);
}

static void beep_confirm(void)
{
    drv_buzzer_beep(BUZZER_TONE_CLICK, BEEP_CONFIRM_MS);
}

/* Converts the continuously-accumulating raw quadrature count into whole
 * mechanical detents moved since the last call. *last_pos holds the last
 * reported detent index (0 at init).
 *
 * encoder_counts_per_detent is EEPROM-backed and NOT confirmed against
 * this board's actual encoder part (2026-08-17 user note: "not even sure
 * about the edges per dent") — that's why it's a settings field, tunable
 * after bring-up without a reflash. */
static int16_t consume_detents(int32_t current_count, int32_t *last_pos)
{
    /* Snap the raw quadrature count to the NEAREST whole detent index and
     * report how many detents that moved since last call. Round-to-nearest
     * (not truncate) means a click registers as the knob passes the
     * half-way point between mechanical detents, regardless of where in
     * the Gray cycle the encoder happened to rest at power-on — so the
     * first turn always counts, no "2 turns to react". It's also self-
     * correcting: because pos is derived from the absolute count, a single
     * missed or bounced edge can't permanently shift the phase.
     *
     * per_detent comes from g_device_settings.encoder_counts_per_detent
     * (EEPROM-backed; svc_storage_init() guarantees it nonzero before
     * app_ui_init() runs). */
    int32_t per  = (int32_t)g_device_settings.encoder_counts_per_detent;
    int32_t half = per / 2;
    int32_t pos  = (current_count >= 0)
                     ? ( (current_count + half) / per)
                     : -(((-current_count) + half) / per);
    int16_t steps = (int16_t)(pos - *last_pos);
    *last_pos = pos;
    return steps;
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
    s_enc1_pos = 0;
    s_enc2_pos = 0;

    g_ui_state.current_screen   = UI_SCREEN_LIVE;
    g_ui_state.previous_screen  = UI_SCREEN_LIVE;
    g_ui_state.settings_cursor  = 0;
    g_ui_state.settings_editing = false;
    g_ui_state.redraw_needed    = true;
}

void app_ui_update(void)
{
    int16_t e1_steps = consume_detents(g_system_state.encoder1_count, &s_enc1_pos);
    int16_t e2_steps = consume_detents(g_system_state.encoder2_count, &s_enc2_pos);
    bool    e1_press = g_system_state.encoder1_sw_press_event;
    bool    e2_press = g_system_state.encoder2_sw_press_event;
    g_system_state.encoder1_sw_press_event = false;
    g_system_state.encoder2_sw_press_event = false;

    /* ---- LEFT encoder: screen-level navigation ----
     * Rotate = move between screens (LIVE <-> STATUS <-> SETTINGS).
     * Ignored while editing a value so you can't accidentally jump away
     * mid-edit. Push = back / cancel. */
    if (!g_ui_state.settings_editing) {
        if (e2_steps > 0) {
            switch_screen(next_screen(g_ui_state.current_screen));
            beep_nav(e2_steps);
        } else if (e2_steps < 0) {
            switch_screen(prev_screen(g_ui_state.current_screen));
            beep_nav(e2_steps);
        }
    }
    if (e2_press) {
        beep_confirm();
        if (g_ui_state.settings_editing) {
            cancel_edit();
        } else {
            switch_screen(g_ui_state.previous_screen);
        }
    }

    /* ---- RIGHT encoder: navigation WITHIN the current screen ----
     * SETTINGS+editing: rotate changes the value, push commits.
     * SETTINGS not editing: rotate moves the cursor, push enters edit.
     * LIVE / STATUS: nothing to navigate within. */
    if (g_ui_state.current_screen == UI_SCREEN_SETTINGS && g_ui_state.settings_editing) {
        if (e1_steps != 0) {
            const UiSettingMeta *m = app_ui_setting_meta((UiSettingIndex)g_ui_state.settings_cursor);
            int32_t delta = (int32_t)e1_steps * m->step;
            g_ui_state.edit_value = clamp(g_ui_state.edit_value + delta, m->min_v, m->max_v);
            beep_nav(e1_steps);
            g_ui_state.redraw_needed = true;
        }
        if (e1_press) {
            commit_edit();
            beep_confirm();
        }
    } else if (g_ui_state.current_screen == UI_SCREEN_SETTINGS) {
        if (e1_steps > 0) {
            g_ui_state.settings_cursor = (uint8_t)((g_ui_state.settings_cursor + 1U)
                                                   % UI_SETTING_COUNT);
            beep_nav(e1_steps);
            g_ui_state.redraw_needed = true;
        } else if (e1_steps < 0) {
            g_ui_state.settings_cursor =
                (uint8_t)((g_ui_state.settings_cursor + UI_SETTING_COUNT - 1U)
                          % UI_SETTING_COUNT);
            beep_nav(e1_steps);
            g_ui_state.redraw_needed = true;
        }
        if (e1_press) {
            enter_edit_mode();
            beep_confirm();
        }
    }
    /* LIVE / STATUS: RIGHT encoder rotate/push do nothing in WP3. */
}
