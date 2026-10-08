#include "math_supervisor.h"

void math_supervisor_init(MathSupervisor *s, uint32_t progress_now)
{
    s->last_progress = progress_now;
    s->stall_ms      = 0;
    s->tripped       = false;
}

bool math_supervisor_tick(MathSupervisor *s, uint32_t progress, uint32_t stall_limit_ms)
{
    if (s->tripped) {
        return false;
    }
    if (progress != s->last_progress) {
        s->last_progress = progress;
        s->stall_ms      = 0;
        return true;
    }
    if (++s->stall_ms >= stall_limit_ms) {
        s->tripped = true;
        return false;
    }
    return true;
}
