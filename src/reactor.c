#include "chaaya/reactor.h"

#include <string.h>
#include <time.h>

#if defined(_WIN32)
#include <windows.h>
#endif

static void sleep_ms(uint64_t ms) {
    if (ms == 0) {
        return;
    }
#if defined(_WIN32)
    if (ms > UINT32_MAX) {
        ms = UINT32_MAX;
    }
    Sleep((DWORD)ms);
#else
    struct timespec req;
    req.tv_sec = (time_t)(ms / 1000);
    req.tv_nsec = (long)((ms % 1000) * 1000000ULL);
    (void)nanosleep(&req, NULL);
#endif
}

void ch_reactor_init(ChReactor *reactor) {
    if (!reactor) {
        return;
    }
    memset(reactor, 0, sizeof(*reactor));
    reactor->next_id = 1;
}

void ch_reactor_deinit(ChReactor *reactor) {
    if (!reactor) {
        return;
    }
    memset(reactor, 0, sizeof(*reactor));
}

uint64_t ch_reactor_now_ms(void) {
    struct timespec ts;
    if (timespec_get(&ts, TIME_UTC) == 0) {
        return 0;
    }
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

int ch_reactor_schedule(ChReactor *reactor, uint64_t delay_ms, ChValue payload, uint64_t *out_id) {
    if (!reactor) {
        return -1;
    }
    for (size_t i = 0; i < CH_REACTOR_MAX_TIMERS; i++) {
        if (reactor->timers[i].active) {
            continue;
        }
        uint64_t now = ch_reactor_now_ms();
        reactor->timers[i].active = true;
        reactor->timers[i].id = reactor->next_id++;
        reactor->timers[i].due_ms = now + delay_ms;
        reactor->timers[i].payload = payload;
        reactor->timer_count++;
        if (out_id) {
            *out_id = reactor->timers[i].id;
        }
        return 0;
    }
    return -1;
}

int ch_reactor_cancel(ChReactor *reactor, uint64_t id) {
    if (!reactor) {
        return -1;
    }
    for (size_t i = 0; i < CH_REACTOR_MAX_TIMERS; i++) {
        if (!reactor->timers[i].active || reactor->timers[i].id != id) {
            continue;
        }
        reactor->timers[i].active = false;
        reactor->timers[i].payload = CH_UNDEFINED;
        if (reactor->timer_count > 0) {
            reactor->timer_count--;
        }
        return 0;
    }
    return -1;
}

static ptrdiff_t earliest_ready_timer(const ChReactor *reactor, uint64_t now_ms,
                                      uint64_t *next_due_ms) {
    ptrdiff_t ready_index = -1;
    uint64_t min_due = UINT64_MAX;
    for (size_t i = 0; i < CH_REACTOR_MAX_TIMERS; i++) {
        const ChReactorTimer *timer = &reactor->timers[i];
        if (!timer->active) {
            continue;
        }
        if (timer->due_ms <= now_ms) {
            return (ptrdiff_t)i;
        }
        if (timer->due_ms < min_due) {
            min_due = timer->due_ms;
        }
    }
    if (next_due_ms && min_due != UINT64_MAX) {
        *next_due_ms = min_due;
    }
    return ready_index;
}

int ch_reactor_poll(ChReactor *reactor, uint64_t timeout_ms, ChValue *out_payload) {
    if (!reactor) {
        return -1;
    }
    uint64_t start_ms = ch_reactor_now_ms();
    uint64_t deadline_ms = start_ms + timeout_ms;

    for (;;) {
        uint64_t now_ms = ch_reactor_now_ms();
        uint64_t next_due_ms = 0;
        ptrdiff_t idx = earliest_ready_timer(reactor, now_ms, &next_due_ms);
        if (idx >= 0) {
            ChReactorTimer *timer = &reactor->timers[(size_t)idx];
            if (out_payload) {
                *out_payload = timer->payload;
            }
            timer->active = false;
            timer->payload = CH_UNDEFINED;
            if (reactor->timer_count > 0) {
                reactor->timer_count--;
            }
            return 1;
        }

        if (timeout_ms == 0 || now_ms >= deadline_ms) {
            return 0;
        }

        uint64_t sleep_until = deadline_ms;
        if (next_due_ms != 0 && next_due_ms < sleep_until) {
            sleep_until = next_due_ms;
        }
        if (sleep_until <= now_ms) {
            continue;
        }
        sleep_ms(sleep_until - now_ms);
    }
}

size_t ch_reactor_push_roots(ChGC *gc, ChReactor *reactor) {
    if (!gc || !reactor) {
        return 0;
    }
    size_t pushed = 0;
    for (size_t i = 0; i < CH_REACTOR_MAX_TIMERS; i++) {
        if (!reactor->timers[i].active) {
            continue;
        }
        ch_gc_push(gc, &reactor->timers[i].payload);
        pushed++;
    }
    return pushed;
}
