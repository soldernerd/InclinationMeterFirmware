#include "svc_power.h"
#include "svc_battery.h"
#include "svc_measurement.h"
#include "svc_log.h"
#include "hal_systick.h"
#include "system_state.h"
#include "config.h"

/* See svc_power.h. Activity is detected without any cooperation from the
 * UI layer: the encoder rotation accumulators and the raw switch levels
 * in g_system_state are snapshotted each tick, and any change resets the
 * idle timer. That covers every rotation and every press/release edge
 * (the latched *_press_event flags, which app_ui.c consumes, are
 * deliberately not used here). */

static int32_t  s_enc1_seen;
static int32_t  s_enc2_seen;
static bool     s_sw1_seen;
static bool     s_sw2_seen;
static uint32_t s_idle_since_ms;

static bool activity_seen(void)
{
    bool changed = (g_system_state.encoder1_count      != s_enc1_seen)
                || (g_system_state.encoder2_count      != s_enc2_seen)
                || (g_system_state.encoder1_sw_pressed != s_sw1_seen)
                || (g_system_state.encoder2_sw_pressed != s_sw2_seen);

    s_enc1_seen = g_system_state.encoder1_count;
    s_enc2_seen = g_system_state.encoder2_count;
    s_sw1_seen  = g_system_state.encoder1_sw_pressed;
    s_sw2_seen  = g_system_state.encoder2_sw_pressed;
    return changed;
}

/* Something else is going on that should keep the device awake regardless
 * of the encoders sitting still. */
static bool suppressed(void)
{
    return g_system_state.usb_connected
        || g_system_state.ble_connected
        || g_system_state.battery_charging
        || (svc_measurement_get_state() != MEAS_STATE_IDLE);
}

void svc_power_init(void)
{
    s_enc1_seen     = g_system_state.encoder1_count;
    s_enc2_seen     = g_system_state.encoder2_count;
    s_sw1_seen      = g_system_state.encoder1_sw_pressed;
    s_sw2_seen      = g_system_state.encoder2_sw_pressed;
    s_idle_since_ms = hal_systick_get_ms();
}

void svc_power_task(void)
{
    uint32_t now = hal_systick_get_ms();

    if (activity_seen() || suppressed()) {
        s_idle_since_ms = now;
        return;
    }

    uint16_t limit_s = g_device_settings.auto_poweroff_s;
    if (limit_s == 0U) {
        return;                                   /* auto power-off disabled */
    }

    if ((uint32_t)(now - s_idle_since_ms) >= (uint32_t)limit_s * 1000U) {
        svc_log(API2_LOG_INFO, "power: auto-off (idle timeout)");
        svc_battery_enter_low_power();             /* never returns */
    }
}

void svc_power_shutdown_now(void)
{
    svc_log(API2_LOG_INFO, "power: user power-off");
    svc_battery_enter_low_power();                 /* never returns */
}
