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

extern UiState g_ui_state;

void app_ui_init(void);
void app_ui_update(void);

/* Reads the live value of one setting from g_device_settings. Exposed so
 * App/app_display.c can show the same value it's driven by, rather than
 * re-implementing the UiSettingIndex -> field switch itself (that
 * duplication used to exist and could silently desync). */
int32_t app_ui_setting_read(UiSettingIndex i);

#endif /* APP_UI_H */
