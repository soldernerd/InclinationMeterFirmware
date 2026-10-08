/* Host tests for Services/svc_service.c -- service mode: entered only locally, ended by leave, idle timeout, sleep. */
#include "test.h"

#include "../Config/config.h"

static unsigned g_ms;
#include <stdint.h>
uint32_t hal_systick_get_ms(void) { return g_ms; }

#include "../Services/svc_log.h"
static int g_logs, g_warns;
void svc_log(Api2LogSeverity sev, const char *msg) { (void)msg; g_logs++; if (sev == API2_LOG_WARN) g_warns++; }
void svc_logf(Api2LogSeverity sev, const char *fmt, ...) { (void)sev; (void)fmt; g_logs++; }

#include "../Services/svc_service.c"

static void fresh(void)
{
    g_ms = 100000u;
    g_logs = g_warns = 0;
    svc_service_init();
}

TEST(it_starts_locked_and_unlocks_only_by_enter)
{
    fresh();
    CHECK(!svc_service_active());
    CHECK_EQ(svc_service_seconds_left(), 0);
    svc_service_note_api_activity();                 /* activity alone never unlocks it */
    svc_service_update();
    CHECK(!svc_service_active());
    svc_service_enter();
    CHECK(svc_service_active());
    CHECK_EQ(svc_service_seconds_left(), 600);
    CHECK_EQ(g_warns, 1);                            /* entering is announced in the log */
}

TEST(leaving_ends_it_at_once_and_is_idempotent)
{
    fresh();
    svc_service_enter();
    svc_service_leave();
    CHECK(!svc_service_active());
    int logs = g_logs;
    svc_service_leave();
    CHECK_EQ(g_logs, logs);                          /* a second leave is silent */
}

TEST(entering_twice_does_not_restart_the_timeout)
{
    fresh();
    svc_service_enter();
    g_ms += 100000u;
    svc_service_enter();
    CHECK_EQ(svc_service_seconds_left(), 500);
}

TEST(it_ends_after_ten_minutes_without_api_activity)
{
    fresh();
    svc_service_enter();
    g_ms += 599999u;
    svc_service_update();
    CHECK(svc_service_active());
    CHECK_EQ(svc_service_seconds_left(), 1);
    g_ms += 1u;
    svc_service_update();
    CHECK(!svc_service_active());
}

TEST(every_api_request_restarts_the_idle_timeout)
{
    fresh();
    svc_service_enter();
    for (int i = 0; i < 20; ++i) {                   /* a request every 9 minutes for three hours */
        g_ms += 540000u;
        svc_service_note_api_activity();
        svc_service_update();
        CHECK(svc_service_active());
    }
    CHECK_EQ(svc_service_seconds_left(), 600);
    g_ms += 600000u;
    svc_service_update();
    CHECK(!svc_service_active());
}

TEST(the_timer_survives_the_tick_counter_wrapping)
{
    fresh();
    g_ms = 0xFFFFFFFFu - 1000u;
    svc_service_enter();
    g_ms += 599000u;                                 /* wraps past zero */
    svc_service_update();
    CHECK(svc_service_active());
    g_ms += 1500u;
    svc_service_update();
    CHECK(!svc_service_active());
}

TEST(a_new_run_after_leaving_starts_a_fresh_timeout)
{
    fresh();
    svc_service_enter();
    g_ms += 300000u;
    svc_service_leave();
    g_ms += 1000u;
    svc_service_enter();
    CHECK_EQ(svc_service_seconds_left(), 600);
}

int main(void)
{
    RUN(it_starts_locked_and_unlocks_only_by_enter);
    RUN(leaving_ends_it_at_once_and_is_idempotent);
    RUN(entering_twice_does_not_restart_the_timeout);
    RUN(it_ends_after_ten_minutes_without_api_activity);
    RUN(every_api_request_restarts_the_idle_timeout);
    RUN(the_timer_survives_the_tick_counter_wrapping);
    RUN(a_new_run_after_leaving_starts_a_fresh_timeout);
    return test_summary();
}
