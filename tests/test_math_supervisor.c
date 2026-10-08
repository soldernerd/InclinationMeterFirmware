/* Host tests for Math/math_supervisor.c -- when the SysTick interrupt may refresh the hardware watchdog. */
#include "test.h"

#include "../Math/math_supervisor.c"

#define LIMIT 3000U

TEST(a_main_loop_that_keeps_advancing_is_never_cut_off)
{
    MathSupervisor s;
    math_supervisor_init(&s, 0);
    uint32_t progress = 0;
    for (uint32_t ms = 0; ms < 100000; ++ms) {
        if ((ms % 2000U) == 0) progress++;           /* a slow loop: one pass every 2 s, below the limit */
        CHECK(math_supervisor_tick(&s, progress, LIMIT));
    }
    CHECK(!s.tripped);
}

TEST(a_stalled_main_loop_stops_the_refreshes_exactly_at_the_limit)
{
    MathSupervisor s;
    math_supervisor_init(&s, 5);
    for (uint32_t ms = 1; ms < LIMIT; ++ms) {
        CHECK(math_supervisor_tick(&s, 5, LIMIT));   /* LIMIT - 1 stalled milliseconds are tolerated */
    }
    CHECK(!math_supervisor_tick(&s, 5, LIMIT));      /* the LIMIT-th is not */
    CHECK(s.tripped);
}

TEST(once_tripped_it_never_recovers)
{
    MathSupervisor s;
    math_supervisor_init(&s, 0);
    for (uint32_t ms = 0; ms < LIMIT; ++ms) math_supervisor_tick(&s, 0, LIMIT);
    CHECK(s.tripped);
    CHECK(!math_supervisor_tick(&s, 1, LIMIT));      /* a late sign of life is too late: the reset is already coming */
    CHECK(!math_supervisor_tick(&s, 2, LIMIT));
}

TEST(progress_just_before_the_limit_resets_the_stall_time)
{
    MathSupervisor s;
    math_supervisor_init(&s, 0);
    for (uint32_t ms = 0; ms < LIMIT - 1; ++ms) CHECK(math_supervisor_tick(&s, 0, LIMIT));
    CHECK(math_supervisor_tick(&s, 1, LIMIT));
    for (uint32_t ms = 0; ms < LIMIT - 1; ++ms) CHECK(math_supervisor_tick(&s, 1, LIMIT));
    CHECK(!s.tripped);
}

TEST(the_counter_wrapping_around_counts_as_progress)
{
    MathSupervisor s;
    math_supervisor_init(&s, 0xFFFFFFFFu);
    CHECK(math_supervisor_tick(&s, 0, LIMIT));       /* 0xFFFFFFFF -> 0 is a change */
    CHECK_EQ(s.stall_ms, 0);
}

int main(void)
{
    RUN(a_main_loop_that_keeps_advancing_is_never_cut_off);
    RUN(a_stalled_main_loop_stops_the_refreshes_exactly_at_the_limit);
    RUN(once_tripped_it_never_recovers);
    RUN(progress_just_before_the_limit_resets_the_stall_time);
    RUN(the_counter_wrapping_around_counts_as_progress);
    return test_summary();
}
