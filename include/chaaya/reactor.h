#ifndef CHAAYA_REACTOR_H
#define CHAAYA_REACTOR_H

#include "chaaya/gc.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CH_REACTOR_MAX_TIMERS 64

typedef struct ChReactorTimer {
    uint64_t id;
    uint64_t due_ms;
    ChValue payload;
    bool active;
} ChReactorTimer;

typedef struct ChReactor {
    ChReactorTimer timers[CH_REACTOR_MAX_TIMERS];
    size_t timer_count;
    uint64_t next_id;
} ChReactor;

void ch_reactor_init(ChReactor *reactor);
void ch_reactor_deinit(ChReactor *reactor);

uint64_t ch_reactor_now_ms(void);
int ch_reactor_schedule(ChReactor *reactor, uint64_t delay_ms, ChValue payload, uint64_t *out_id);
int ch_reactor_cancel(ChReactor *reactor, uint64_t id);
int ch_reactor_poll(ChReactor *reactor, uint64_t timeout_ms, ChValue *out_payload);

/* Push active timer payload slots onto the root stack. Returns pushed count. */
size_t ch_reactor_push_roots(ChGC *gc, ChReactor *reactor);

#ifdef __cplusplus
}
#endif

#endif /* CHAAYA_REACTOR_H */
