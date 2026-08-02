#include "chaaya/reactor.h"

#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
#include <sys/event.h>
#include <sys/time.h>
#include <unistd.h>
#define CH_REACTOR_KQUEUE 1
#elif defined(__linux__)
#include <sys/epoll.h>
#include <unistd.h>
#define CH_REACTOR_EPOLL 1
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
    reactor->backend_fd = -1;
#if defined(CH_REACTOR_KQUEUE)
    reactor->backend_fd = kqueue();
#elif defined(CH_REACTOR_EPOLL)
    reactor->backend_fd = epoll_create1(0);
#endif
}

void ch_reactor_deinit(ChReactor *reactor) {
    if (!reactor) {
        return;
    }
#if !defined(_WIN32)
    if (reactor->backend_fd >= 0) {
        close(reactor->backend_fd);
    }
#endif
    memset(reactor, 0, sizeof(*reactor));
    reactor->backend_fd = -1;
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

int ch_reactor_register_fd(ChReactor *reactor, int fd, ChReactorInterest interest,
                           ChValue payload) {
    if (!reactor || fd < 0) {
        return -1;
    }
    for (size_t i = 0; i < CH_REACTOR_MAX_FDS; i++) {
        if (reactor->fds[i].active) {
            continue;
        }
#if defined(CH_REACTOR_KQUEUE)
        if (reactor->backend_fd >= 0) {
            struct kevent ev;
            short filter = (interest == CH_REACTOR_WRITE) ? EVFILT_WRITE : EVFILT_READ;
            EV_SET(&ev, fd, filter, EV_ADD | EV_ONESHOT, 0, 0, NULL);
            if (kevent(reactor->backend_fd, &ev, 1, NULL, 0, NULL) < 0) {
                return -1;
            }
        }
#elif defined(CH_REACTOR_EPOLL)
        if (reactor->backend_fd >= 0) {
            struct epoll_event ev;
            memset(&ev, 0, sizeof(ev));
            ev.events = (interest == CH_REACTOR_WRITE) ? EPOLLOUT : EPOLLIN;
            ev.events |= EPOLLONESHOT;
            ev.data.fd = fd;
            if (epoll_ctl(reactor->backend_fd, EPOLL_CTL_ADD, fd, &ev) < 0) {
                if (errno == EEXIST) {
                    if (epoll_ctl(reactor->backend_fd, EPOLL_CTL_MOD, fd, &ev) < 0) {
                        return -1;
                    }
                } else {
                    return -1;
                }
            }
        }
#endif
        reactor->fds[i].active = 1;
        reactor->fds[i].fd = fd;
        reactor->fds[i].interest = (uint8_t)interest;
        reactor->fds[i].payload = payload;
        reactor->fd_count++;
        return 0;
    }
    return -1;
}

int ch_reactor_unregister_fd(ChReactor *reactor, int fd) {
    if (!reactor) {
        return -1;
    }
    for (size_t i = 0; i < CH_REACTOR_MAX_FDS; i++) {
        if (!reactor->fds[i].active || reactor->fds[i].fd != fd) {
            continue;
        }
#if defined(CH_REACTOR_EPOLL)
        if (reactor->backend_fd >= 0) {
            (void)epoll_ctl(reactor->backend_fd, EPOLL_CTL_DEL, fd, NULL);
        }
#endif
        reactor->fds[i].active = 0;
        reactor->fds[i].payload = CH_UNDEFINED;
        if (reactor->fd_count > 0) {
            reactor->fd_count--;
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

static int poll_fds(ChReactor *reactor, uint64_t wait_ms, ChValue *out_payload) {
    if (!reactor || reactor->backend_fd < 0 || reactor->fd_count == 0) {
        return 0;
    }
#if defined(CH_REACTOR_KQUEUE)
    struct kevent ev;
    struct timespec ts;
    ts.tv_sec = (time_t)(wait_ms / 1000);
    ts.tv_nsec = (long)((wait_ms % 1000) * 1000000ULL);
    int n = kevent(reactor->backend_fd, NULL, 0, &ev, 1, &ts);
    if (n <= 0) {
        return 0;
    }
    int fd = (int)ev.ident;
    for (size_t i = 0; i < CH_REACTOR_MAX_FDS; i++) {
        if (!reactor->fds[i].active || reactor->fds[i].fd != fd) {
            continue;
        }
        if (out_payload) {
            *out_payload = reactor->fds[i].payload;
        }
        reactor->fds[i].active = 0;
        reactor->fds[i].payload = CH_UNDEFINED;
        if (reactor->fd_count > 0) {
            reactor->fd_count--;
        }
        return 1;
    }
#elif defined(CH_REACTOR_EPOLL)
    struct epoll_event ev;
    int n = epoll_wait(reactor->backend_fd, &ev, 1, (int)(wait_ms > INT_MAX ? INT_MAX : wait_ms));
    if (n <= 0) {
        return 0;
    }
    int fd = ev.data.fd;
    for (size_t i = 0; i < CH_REACTOR_MAX_FDS; i++) {
        if (!reactor->fds[i].active || reactor->fds[i].fd != fd) {
            continue;
        }
        if (out_payload) {
            *out_payload = reactor->fds[i].payload;
        }
        reactor->fds[i].active = 0;
        reactor->fds[i].payload = CH_UNDEFINED;
        if (reactor->fd_count > 0) {
            reactor->fd_count--;
        }
        return 1;
    }
#else
    (void)wait_ms;
    (void)out_payload;
#endif
    return 0;
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

        uint64_t wait_ms = 0;
        if (timeout_ms == 0) {
            wait_ms = 0;
        } else if (now_ms >= deadline_ms) {
            return 0;
        } else {
            wait_ms = deadline_ms - now_ms;
            if (next_due_ms != 0 && next_due_ms > now_ms && (next_due_ms - now_ms) < wait_ms) {
                wait_ms = next_due_ms - now_ms;
            }
        }

        if (reactor->fd_count > 0 && reactor->backend_fd >= 0) {
            if (poll_fds(reactor, wait_ms, out_payload) > 0) {
                return 1;
            }
        } else if (wait_ms > 0) {
            sleep_ms(wait_ms);
        } else {
            return 0;
        }
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
    for (size_t i = 0; i < CH_REACTOR_MAX_FDS; i++) {
        if (!reactor->fds[i].active) {
            continue;
        }
        ch_gc_push(gc, &reactor->fds[i].payload);
        pushed++;
    }
    return pushed;
}
