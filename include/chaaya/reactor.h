#ifndef CHAAYA_REACTOR_H
#define CHAAYA_REACTOR_H

#include "chaaya/gc.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CH_REACTOR_MAX_TIMERS 64
#define CH_REACTOR_MAX_FDS 64

typedef enum ChReactorInterest {
    CH_REACTOR_READ = 1,
    CH_REACTOR_WRITE = 2,
} ChReactorInterest;

typedef struct ChReactorTimer {
    uint64_t id;
    uint64_t due_ms;
    ChValue payload;
    bool active;
} ChReactorTimer;

typedef struct ChReactorFd {
    int fd;
    uint8_t interest;
    uint8_t active;
    ChValue payload;
} ChReactorFd;

typedef struct ChReactor {
    ChReactorTimer timers[CH_REACTOR_MAX_TIMERS];
    size_t timer_count;
    uint64_t next_id;
    ChReactorFd fds[CH_REACTOR_MAX_FDS];
    size_t fd_count;
    int backend_fd; /* kqueue or epoll fd; -1 if unused */
} ChReactor;

void ch_reactor_init(ChReactor *reactor);
void ch_reactor_deinit(ChReactor *reactor);

uint64_t ch_reactor_now_ms(void);
int ch_reactor_schedule(ChReactor *reactor, uint64_t delay_ms, ChValue payload, uint64_t *out_id);
int ch_reactor_cancel(ChReactor *reactor, uint64_t id);
int ch_reactor_register_fd(ChReactor *reactor, int fd, ChReactorInterest interest, ChValue payload);
int ch_reactor_unregister_fd(ChReactor *reactor, int fd);
/* Poll timers and fds. Returns 1 and sets *out_payload when an event fires. */
int ch_reactor_poll(ChReactor *reactor, uint64_t timeout_ms, ChValue *out_payload);

/* Push active timer/fd payload slots onto the root stack. Returns pushed count. */
size_t ch_reactor_push_roots(ChGC *gc, ChReactor *reactor);

#ifdef __cplusplus
}
#endif

#endif /* CHAAYA_REACTOR_H */
