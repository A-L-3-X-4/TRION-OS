#pragma once
#include "kernel.h"
#include "sched.h"
#include "timer.h"
#include "abi.h"

struct deadline {
    uint64_t timeout_ms;
    uint64_t end_ns;
};

static inline void deadline_init(struct deadline *d, uint64_t timeout_ms)
{
    if (timeout_ms >= (1ULL << 40))
        timeout_ms = IPC_TIMEOUT_INF;
    d->timeout_ms = timeout_ms;
    d->end_ns = (timeout_ms == IPC_TIMEOUT_INF || timeout_ms == 0)
                    ? 0 : clock_ns() + timeout_ms * 1000000ULL;
}

static inline bool deadline_left(const struct deadline *d, uint64_t *ms)
{
    if (d->timeout_ms == IPC_TIMEOUT_INF) {
        *ms = SEM_FOREVER;
        return true;
    }
    uint64_t now = clock_ns();
    if (d->timeout_ms == 0 || now >= d->end_ns)
        return false;
    *ms = (d->end_ns - now + 999999) / 1000000;
    return true;
}

static inline int would_block_error(const struct deadline *d)
{
    return d->timeout_ms == 0 ? -E_AGAIN : -E_TIMEDOUT;
}
