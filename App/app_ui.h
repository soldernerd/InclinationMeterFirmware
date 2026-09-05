#ifndef APP_UI_H
#define APP_UI_H

#include <stdint.h>
#include <stdbool.h>

typedef enum {
    UI_SCREEN_LIVE = 0,
    UI_SCREEN_STATUS,
    UI_SCREEN_SETTINGS,
    UI_SCREEN_COUNT,
} UiScreen;

/* Identifiers for editable settings on the SETTINGS screen.
 * Order here is the visual order on screen. */
typedef enum {
    UI_SETTING_DISPLAY_RATE = 0,
    UI_SETTING_BATTERY_CRITICAL,   /* battery_critical_mv — REV B renamed
                                     * this from the original WP3 spec's
                                     * "battery cutoff"; see config.h */
    UI_SETTING_STREAM_INTERVAL,
    UI_SETTING_SETTLING_TIMEOUT,
    UI_SETTING_AUTO_POWEROFF,       /* auto_poweroff_s (WP6); 0 = disabled */
    UI_SETTING_REBOOT_DFU,          /* action, not a value — see its
                                     * UiSettingMeta.step == 0 and
                                     * app_ui.c's app_ui_update() */
    UI_SETTING_POWER_OFF,           /* action — Standby power-off */
    UI_SETTING_COUNT,
} UiSettingIndex;

typedef struct {
    UiScreen current_screen;
    UiScreen previous_screen;
    uint8_t  settings_cursor;       /* 0..UI_SETTING_COUNT-1 */
    bool     settings_editing;      /* encoder1 adjusts value */
    bool     redraw_needed;         /* set by ui — checked & cleared by display */
    int32_t  edit_value;            /* working copy while editing */
} UiState;

/* Display label/unit/edit-range for one setting — single source of truth
 * shared by App/app_ui.c (edit clamping) and App/app_display.c (rendering
 * the SETTINGS screen), instead of each keeping its own copy.
 *
 * step == 0 marks an ACTION row (currently only UI_SETTING_REBOOT_DFU)
 * rather than a numeric value: unit/min_v/max_v are unused, rotating the
 * encoder does nothing, and a second RIGHT press while "editing" performs
 * the action immediately instead of committing a value to EEPROM. */
typedef struct {
    const char *label;
    const char *unit;
    int32_t     step;
    int32_t     min_v;
    int32_t     max_v;
} UiSettingMeta;

extern UiState g_ui_state;

void app_ui_init(void);
void app_ui_update(void);

/* Reads the live value of one setting from g_device_settings. Exposed so
 * App/app_display.c can show the same value it's driven by, rather than
 * re-implementing the UiSettingIndex -> field switch itself (that
 * duplication used to exist and could silently desync). */
int32_t app_ui_setting_read(UiSettingIndex i);

/* Never returns NULL — an out-of-range index falls back to index 0
 * rather than crash a display routine over a cursor bug. */
const UiSettingMeta *app_ui_setting_meta(UiSettingIndex i);

#endif /* APP_UI_H */
