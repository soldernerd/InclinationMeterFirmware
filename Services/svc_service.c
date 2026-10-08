#include "svc_service.h"
#include "svc_log.h"
#include "config.h"
#include "hal_systick.h"

static bool     s_active = false;
static uint32_t s_last_activity_ms = 0;

void svc_service_init(void)
{
    s_active = false;
}

bool svc_service_active(void)
{
    return s_active;
}

void svc_service_enter(void)
{
    if (s_active) {
        return;
    }
    s_active = true;
    s_last_activity_ms = hal_systick_get_ms();
    svc_log(API2_LOG_WARN, "service mode ON (calibration writes and maintenance commands unlocked)");
}

void svc_service_leave(void)
{
    if (!s_active) {
        return;
    }
    s_active = false;
    svc_log(API2_LOG_INFO, "service mode OFF");
}

void svc_service_note_api_activity(void)
{
    if (s_active) {
        s_last_activity_ms = hal_systick_get_ms();
    }
}

void svc_service_update(void)
{
    if (s_active && (uint32_t)(hal_systick_get_ms() - s_last_activity_ms) >= SERVICE_MODE_IDLE_TIMEOUT_MS) {
        s_active = false;
        svc_log(API2_LOG_INFO, "service mode OFF (no API activity)");
    }
}

uint16_t svc_service_seconds_left(void)
{
    if (!s_active) {
        return 0;
    }
    uint32_t elapsed = (uint32_t)(hal_systick_get_ms() - s_last_activity_ms);
    if (elapsed >= SERVICE_MODE_IDLE_TIMEOUT_MS) {
        return 0;
    }
    return (uint16_t)((SERVICE_MODE_IDLE_TIMEOUT_MS - elapsed + 999U) / 1000U);
}
