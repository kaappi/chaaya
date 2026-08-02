#ifndef CHAAYA_REACTOR_H
#define CHAAYA_REACTOR_H

#include "chaaya/gc.h"

#include <stdatomic.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CH_REACTOR_MAX_TIMERS 64
#define CH_REACTOR_MAX_FDS 64

/* Cross-thread wakeup handle, one per ChReactor (one per OS thread's fiber
 * runtime). Registered into ChSharedChannel waiter rings so a remote
 * thread's send/recv/close can wake this thread out of ch_reactor_poll
 * immediately instead of waiting for the next poll timeout (KEP-0002 §5
 * "ThreadNotifier" shape, adapted for Chaaya).
 *
 * Lifetime is refcounted independently of the owning ChReactor: other
 * threads may still hold a registration (and thus a reference) after the
 * owning reactor has been deinitialized. `alive` gates only whether
 * ch_thread_notifier_notify() still touches the backend OS resource (which
 * may be mid-teardown); it does not gate memory safety, which the refcount
 * alone provides. */
typedef struct ChThreadNotifier {
    _Atomic uint32_t refcount;
    _Atomic uint8_t wake_pending;
    _Atomic uint8_t alive;
    int backend_fd; /* eventfd (Linux) or the shared kqueue fd (Darwin/BSD); -1 if unsupported */
    int is_kqueue;  /* 1 when backend_fd is a kqueue fd triggered via EVFILT_USER */
} ChThreadNotifier;

typedef enum ChReactorInterest {
    CH_REACTOR_READ = 1,
    CH_REACTOR_WRITE = 2,
} ChReactorInterest;

typedef struct ChReactorTimer {
    uint64_t id;
    uint64_t due_ns;
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
    /* Binary min-heap over active timer slots, ordered by due_ns. heap[]
     * holds slot indices into timers[]; heap_pos[slot] is that slot's
     * current index within heap[] (SIZE_MAX when the slot is not queued),
     * enabling O(log n) cancel-by-id instead of a full rescan. */
    size_t heap[CH_REACTOR_MAX_TIMERS];
    size_t heap_size;
    size_t heap_pos[CH_REACTOR_MAX_TIMERS];
    ChReactorFd fds[CH_REACTOR_MAX_FDS];
    size_t fd_count;
    int backend_fd; /* kqueue or epoll fd; -1 if unused */
    ChThreadNotifier *notifier; /* this thread's cross-thread wakeup handle */
} ChReactor;

void ch_reactor_init(ChReactor *reactor);
void ch_reactor_deinit(ChReactor *reactor);

/* Returns (retaining) this reactor's cross-thread wakeup handle. Callers
 * register the handle in a ChSharedChannel waiter ring and release it when
 * done waiting (or when the channel/notifier is torn down). Thread-safe;
 * the handle itself, once obtained, may be used and released from any
 * thread regardless of what happens to `reactor` afterwards. */
ChThreadNotifier *ch_reactor_notify_handle(ChReactor *reactor);

void ch_thread_notifier_retain(ChThreadNotifier *notifier);
/* Drops one reference; frees the backend resource and the handle itself at
 * the zero transition. Safe to call from any thread. */
void ch_thread_notifier_release(ChThreadNotifier *notifier);
/* Thread-safe wakeup: sets wake_pending then rings the OS primitive so a
 * thread blocked in ch_reactor_poll wakes immediately. A no-op (besides
 * setting wake_pending) once the owning reactor has been deinitialized. */
void ch_thread_notifier_notify(ChThreadNotifier *notifier);

/* Monotonic clock (CLOCK_MONOTONIC where available); never affected by
 * wall-clock adjustments. Suitable for computing relative deadlines. */
uint64_t ch_reactor_now_ns(void);
/* Convenience wrapper: ch_reactor_now_ns() / 1e6. */
uint64_t ch_reactor_now_ms(void);

int ch_reactor_schedule(ChReactor *reactor, uint64_t delay_ms, ChValue payload, uint64_t *out_id);
int ch_reactor_cancel(ChReactor *reactor, uint64_t id);
int ch_reactor_register_fd(ChReactor *reactor, int fd, ChReactorInterest interest, ChValue payload);
int ch_reactor_unregister_fd(ChReactor *reactor, int fd);
/* Poll timers and fds. Returns 1 and sets *out_payload when an event fires. */
int ch_reactor_poll(ChReactor *reactor, uint64_t timeout_ms, ChValue *out_payload);

/* True when there are no active timers and no registered fd waiters, i.e.
 * the reactor has nothing left to wake anyone up. */
int ch_reactor_is_empty(const ChReactor *reactor);
/* Earliest due_ns among active timers, or UINT64_MAX if none are scheduled.
 * O(1) via the heap root; useful for sizing an idle-loop poll timeout. */
uint64_t ch_reactor_earliest_due_ns(const ChReactor *reactor);

/* Push active timer/fd payload slots onto the root stack. Returns pushed count. */
size_t ch_reactor_push_roots(ChGC *gc, ChReactor *reactor);

#ifdef __cplusplus
}
#endif

#endif /* CHAAYA_REACTOR_H */
