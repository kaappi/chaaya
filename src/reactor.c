#include "chaaya/reactor.h"

#include <errno.h>
#include <limits.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdlib.h>
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
#include <sys/eventfd.h>
#include <unistd.h>
#define CH_REACTOR_EPOLL 1
#endif

#define CH_REACTOR_NS_PER_MS 1000000ULL

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

/* --- Timer min-heap (priority by due_ns) -------------------------------- */

static void heap_swap(ChReactor *reactor, size_t i, size_t j) {
    size_t slot_i = reactor->heap[i];
    size_t slot_j = reactor->heap[j];
    reactor->heap[i] = slot_j;
    reactor->heap[j] = slot_i;
    reactor->heap_pos[slot_j] = i;
    reactor->heap_pos[slot_i] = j;
}

static void heap_sift_up(ChReactor *reactor, size_t i) {
    while (i > 0) {
        size_t parent = (i - 1) / 2;
        if (reactor->timers[reactor->heap[parent]].due_ns <=
            reactor->timers[reactor->heap[i]].due_ns) {
            break;
        }
        heap_swap(reactor, parent, i);
        i = parent;
    }
}

static void heap_sift_down(ChReactor *reactor, size_t i) {
    for (;;) {
        size_t left = 2 * i + 1;
        size_t right = 2 * i + 2;
        size_t smallest = i;
        if (left < reactor->heap_size &&
            reactor->timers[reactor->heap[left]].due_ns <
                reactor->timers[reactor->heap[smallest]].due_ns) {
            smallest = left;
        }
        if (right < reactor->heap_size &&
            reactor->timers[reactor->heap[right]].due_ns <
                reactor->timers[reactor->heap[smallest]].due_ns) {
            smallest = right;
        }
        if (smallest == i) {
            break;
        }
        heap_swap(reactor, i, smallest);
        i = smallest;
    }
}

static void heap_push(ChReactor *reactor, size_t slot) {
    size_t i = reactor->heap_size++;
    reactor->heap[i] = slot;
    reactor->heap_pos[slot] = i;
    heap_sift_up(reactor, i);
}

/* Removes the timer occupying `slot` from the heap, wherever it currently
 * sits (not just the root). Used by both cancel-by-id and pop-min. */
static void heap_remove_slot(ChReactor *reactor, size_t slot) {
    size_t pos = reactor->heap_pos[slot];
    if (pos == SIZE_MAX || pos >= reactor->heap_size) {
        return;
    }
    size_t last = reactor->heap_size - 1;
    if (pos != last) {
        heap_swap(reactor, pos, last);
    }
    reactor->heap_size--;
    reactor->heap_pos[slot] = SIZE_MAX;
    if (pos < reactor->heap_size) {
        heap_sift_down(reactor, pos);
        heap_sift_up(reactor, pos);
    }
}

static size_t heap_peek(const ChReactor *reactor) {
    return reactor->heap_size > 0 ? reactor->heap[0] : SIZE_MAX;
}

/* --- ThreadNotifier (KEP-0002 §5 shape, cross-thread wakeup) ------------- */

/* Registers the persistent EVFILT_USER knote (Darwin/BSD) or eventfd
 * (Linux) that ch_thread_notifier_notify() rings from any thread. Returns
 * NULL (leaving the reactor notifier-less; poll() degrades to the old
 * sleep-based idle wait) if the backend has no fd to attach to. */
static ChThreadNotifier *make_notifier(ChReactor *reactor) {
    ChThreadNotifier *n = (ChThreadNotifier *)calloc(1, sizeof(ChThreadNotifier));
    if (!n) {
        return NULL;
    }
    atomic_init(&n->refcount, 1);
    atomic_init(&n->wake_pending, 0);
    atomic_init(&n->alive, 1);
    n->backend_fd = -1;
    n->is_kqueue = 0;
#if defined(CH_REACTOR_KQUEUE)
    if (reactor->backend_fd >= 0) {
        struct kevent ev;
        EV_SET(&ev, 0, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, NULL);
        if (kevent(reactor->backend_fd, &ev, 1, NULL, 0, NULL) == 0) {
            n->backend_fd = reactor->backend_fd;
            n->is_kqueue = 1;
        }
    }
#elif defined(CH_REACTOR_EPOLL)
    if (reactor->backend_fd >= 0) {
        int efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (efd >= 0) {
            struct epoll_event ev;
            memset(&ev, 0, sizeof(ev));
            ev.events = EPOLLIN; /* level-triggered, never ONESHOT: must stay armed */
            ev.data.fd = efd;
            if (epoll_ctl(reactor->backend_fd, EPOLL_CTL_ADD, efd, &ev) == 0) {
                n->backend_fd = efd;
            } else {
                close(efd);
            }
        }
    }
#else
    (void)reactor;
#endif
    return n;
}

ChThreadNotifier *ch_reactor_notify_handle(ChReactor *reactor) {
    if (!reactor || !reactor->notifier) {
        return NULL;
    }
    ch_thread_notifier_retain(reactor->notifier);
    return reactor->notifier;
}

void ch_thread_notifier_retain(ChThreadNotifier *notifier) {
    if (!notifier) {
        return;
    }
    atomic_fetch_add_explicit(&notifier->refcount, 1, memory_order_relaxed);
}

void ch_thread_notifier_release(ChThreadNotifier *notifier) {
    if (!notifier) {
        return;
    }
    if (atomic_fetch_sub_explicit(&notifier->refcount, 1, memory_order_acq_rel) != 1) {
        return;
    }
#if !defined(_WIN32)
    /* Darwin/BSD: backend_fd is the (possibly already-closed-elsewhere)
     * kqueue fd; on Linux it is this notifier's own eventfd. Either way,
     * closing it is this release's job alone once the refcount reaches
     * zero -- never the reactor's, which is what avoids a double-close
     * race on the shared kqueue fd. */
    if (notifier->backend_fd >= 0) {
        close(notifier->backend_fd);
    }
#endif
    free(notifier);
}

void ch_thread_notifier_notify(ChThreadNotifier *notifier) {
    if (!notifier) {
        return;
    }
    atomic_store_explicit(&notifier->wake_pending, 1, memory_order_release);
    if (!atomic_load_explicit(&notifier->alive, memory_order_acquire)) {
        return;
    }
    if (notifier->backend_fd < 0) {
        return;
    }
#if defined(CH_REACTOR_KQUEUE)
    if (notifier->is_kqueue) {
        struct kevent trigger;
        EV_SET(&trigger, 0, EVFILT_USER, 0, NOTE_TRIGGER, 0, NULL);
        struct timespec zero_ts = {0, 0};
        while (kevent(notifier->backend_fd, &trigger, 1, NULL, 0, &zero_ts) < 0 &&
               errno == EINTR) {
        }
    }
#elif defined(CH_REACTOR_EPOLL)
    {
        uint64_t one = 1;
        ssize_t rc;
        do {
            rc = write(notifier->backend_fd, &one, sizeof(one));
        } while (rc < 0 && errno == EINTR);
    }
#endif
}

/* --- Reactor lifecycle --------------------------------------------------- */

void ch_reactor_init(ChReactor *reactor) {
    if (!reactor) {
        return;
    }
    memset(reactor, 0, sizeof(*reactor));
    reactor->next_id = 1;
    reactor->backend_fd = -1;
    for (size_t i = 0; i < CH_REACTOR_MAX_TIMERS; i++) {
        reactor->heap_pos[i] = SIZE_MAX;
    }
#if defined(CH_REACTOR_KQUEUE)
    reactor->backend_fd = kqueue();
#elif defined(CH_REACTOR_EPOLL)
    reactor->backend_fd = epoll_create1(0);
#endif
    reactor->notifier = make_notifier(reactor);
}

void ch_reactor_deinit(ChReactor *reactor) {
    if (!reactor) {
        return;
    }
    /* On the kqueue backends the notifier's EVFILT_USER knote shares this
     * reactor's kqueue fd (see make_notifier), so closing it is exclusively
     * ch_thread_notifier_release's job at the zero-refcount transition --
     * possibly long after this call returns, if another thread still holds
     * a registration. Closing it here unconditionally would yank the fd
     * out from under that other thread. epoll's backend_fd (the epoll
     * instance itself) is never shared cross-thread that way -- only the
     * notifier's own eventfd is -- so it is still closed directly below. */
    int notifier_owns_backend_fd = reactor->notifier && reactor->notifier->is_kqueue;
    if (reactor->notifier) {
        /* Flip alive before releasing: a notify() racing this teardown must
         * see alive==false and skip touching a resource we may be about to
         * hand off (Darwin: the shared kqueue fd) or close (Linux: the
         * eventfd, if this was the last reference). */
        atomic_store_explicit(&reactor->notifier->alive, 0, memory_order_release);
        ch_thread_notifier_release(reactor->notifier);
        reactor->notifier = NULL;
    }
#if !defined(_WIN32)
    if (!notifier_owns_backend_fd && reactor->backend_fd >= 0) {
        close(reactor->backend_fd);
    }
#endif
    memset(reactor, 0, sizeof(*reactor));
    reactor->backend_fd = -1;
}

uint64_t ch_reactor_now_ns(void) {
    struct timespec ts;
#if defined(CLOCK_MONOTONIC)
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
        return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
    }
#endif
    if (timespec_get(&ts, TIME_UTC) != 0) {
        return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
    }
    return 0;
}

uint64_t ch_reactor_now_ms(void) {
    return ch_reactor_now_ns() / CH_REACTOR_NS_PER_MS;
}

int ch_reactor_schedule(ChReactor *reactor, uint64_t delay_ms, ChValue payload, uint64_t *out_id) {
    if (!reactor) {
        return -1;
    }
    for (size_t i = 0; i < CH_REACTOR_MAX_TIMERS; i++) {
        if (reactor->timers[i].active) {
            continue;
        }
        uint64_t now_ns = ch_reactor_now_ns();
        uint64_t delay_ns = (delay_ms > UINT64_MAX / CH_REACTOR_NS_PER_MS)
                                 ? UINT64_MAX
                                 : delay_ms * CH_REACTOR_NS_PER_MS;
        uint64_t due_ns = (delay_ns > UINT64_MAX - now_ns) ? UINT64_MAX : now_ns + delay_ns;
        reactor->timers[i].active = true;
        reactor->timers[i].id = reactor->next_id++;
        reactor->timers[i].due_ns = due_ns;
        reactor->timers[i].payload = payload;
        reactor->timer_count++;
        heap_push(reactor, i);
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
        heap_remove_slot(reactor, i);
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
#if defined(CH_REACTOR_KQUEUE)
        if (reactor->backend_fd >= 0) {
            struct kevent ev;
            short filter =
                (reactor->fds[i].interest == CH_REACTOR_WRITE) ? EVFILT_WRITE : EVFILT_READ;
            EV_SET(&ev, fd, filter, EV_DELETE, 0, 0, NULL);
            /* Ignore errors: EV_ONESHOT entries that already fired are
             * removed by the kernel, so a second delete is a harmless no-op
             * (typically ENOENT). */
            (void)kevent(reactor->backend_fd, &ev, 1, NULL, 0, NULL);
        }
#elif defined(CH_REACTOR_EPOLL)
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

int ch_reactor_is_empty(const ChReactor *reactor) {
    if (!reactor) {
        return 1;
    }
    return reactor->timer_count == 0 && reactor->fd_count == 0;
}

uint64_t ch_reactor_earliest_due_ns(const ChReactor *reactor) {
    if (!reactor) {
        return UINT64_MAX;
    }
    size_t slot = heap_peek(reactor);
    if (slot == SIZE_MAX) {
        return UINT64_MAX;
    }
    return reactor->timers[slot].due_ns;
}

/* Pops the earliest-due timer if it is already due (due_ns <= now_ns).
 * Records the next pending deadline in *next_due_ns (UINT64_MAX if none)
 * regardless of whether a timer fired, so callers can size their wait. */
static ptrdiff_t pop_ready_timer(ChReactor *reactor, uint64_t now_ns, uint64_t *next_due_ns) {
    size_t slot = heap_peek(reactor);
    if (slot == SIZE_MAX) {
        if (next_due_ns) {
            *next_due_ns = UINT64_MAX;
        }
        return -1;
    }
    uint64_t due = reactor->timers[slot].due_ns;
    if (due <= now_ns) {
        return (ptrdiff_t)slot;
    }
    if (next_due_ns) {
        *next_due_ns = due;
    }
    return -1;
}

/* Returns 0 (timeout/nothing), 1 (an fd payload was delivered via
 * *out_payload), or 2 (the thread notifier fired -- no payload; callers
 * should just retry whatever made them poll). Polls even when fd_count==0
 * so the notifier's persistent registration on this same backend fd is
 * never starved by the early-return that used to guard this function. */
static int poll_fds(ChReactor *reactor, uint64_t wait_ms, ChValue *out_payload) {
    if (!reactor || reactor->backend_fd < 0) {
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
    if (ev.filter == EVFILT_USER) {
        /* The notifier's own trigger: EV_CLEAR self-clears the knote, so no
         * separate drain step is needed. Never let ident==0 merge with a
         * real fd 0 (stdin) READ event below -- it is a different filter. */
        if (reactor->notifier) {
            atomic_store_explicit(&reactor->notifier->wake_pending, 0, memory_order_release);
        }
        return 2;
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
    if (reactor->notifier && ev.data.fd == reactor->notifier->backend_fd) {
        /* Level-triggered eventfd, deliberately not ONESHOT (must stay
         * armed): drain here or the next epoll_wait returns immediately
         * forever. */
        uint64_t drain;
        while (read(reactor->notifier->backend_fd, &drain, sizeof(drain)) > 0) {
        }
        atomic_store_explicit(&reactor->notifier->wake_pending, 0, memory_order_release);
        return 2;
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
    uint64_t timeout_ns = (timeout_ms > UINT64_MAX / CH_REACTOR_NS_PER_MS)
                               ? UINT64_MAX
                               : timeout_ms * CH_REACTOR_NS_PER_MS;
    uint64_t start_ns = ch_reactor_now_ns();
    uint64_t deadline_ns =
        (timeout_ns > UINT64_MAX - start_ns) ? UINT64_MAX : start_ns + timeout_ns;

    for (;;) {
        uint64_t now_ns = ch_reactor_now_ns();
        uint64_t next_due_ns = UINT64_MAX;
        ptrdiff_t idx = pop_ready_timer(reactor, now_ns, &next_due_ns);
        if (idx >= 0) {
            size_t slot = (size_t)idx;
            ChReactorTimer *timer = &reactor->timers[slot];
            if (out_payload) {
                *out_payload = timer->payload;
            }
            heap_remove_slot(reactor, slot);
            timer->active = false;
            timer->payload = CH_UNDEFINED;
            if (reactor->timer_count > 0) {
                reactor->timer_count--;
            }
            return 1;
        }

        uint64_t wait_ns;
        if (timeout_ms == 0) {
            wait_ns = 0;
        } else if (now_ns >= deadline_ns) {
            return 0;
        } else {
            wait_ns = deadline_ns - now_ns;
            if (next_due_ns != UINT64_MAX && next_due_ns > now_ns) {
                uint64_t timer_wait_ns = next_due_ns - now_ns;
                if (timer_wait_ns < wait_ns) {
                    wait_ns = timer_wait_ns;
                }
            }
        }
        /* Round up to whole milliseconds: the fd backends and sleep_ms()
         * only accept ms resolution, and rounding down would wake us before
         * the timer is actually due, causing a tight re-poll loop. */
        uint64_t wait_ms = (wait_ns == 0)
                                ? 0
                                : (wait_ns + CH_REACTOR_NS_PER_MS - 1) / CH_REACTOR_NS_PER_MS;

        if (reactor->backend_fd >= 0) {
            /* Always poll the backend, even with fd_count==0: the thread
             * notifier's persistent registration lives on this same fd, so
             * a cross-thread notify() can wake this call immediately.
             * Return values: 1 = fd payload, 2 = the notifier fired (no
             * payload), 0 = plain timeout. */
            int pr = poll_fds(reactor, wait_ms, out_payload);
            if (pr == 1) {
                return 1;
            }
            if (pr == 2) {
                /* Cross-thread notify: hand control back to the caller
                 * right away (0 = "nothing delivered, but re-check") rather
                 * than resuming this same bounded wait for whatever is left
                 * of the original timeout. The whole point of the notifier
                 * is to let the caller's own retry loop re-observe state
                 * (e.g. re-call try_send/try_recv) as soon as a remote
                 * thread rings it, not to keep this call blocked. */
                return 0;
            }
            /* A timeout_ms==0 caller wants exactly one instantaneous check,
             * never a wait: without this, a 0 result here would fall
             * through to the top of the loop and re-issue poll_fds with the
             * same zero wait forever (timeout_ms==0 skips the deadline
             * check above, so nothing else would ever end the loop). A
             * bounded caller (timeout_ms>0) instead loops back around to
             * recheck timers/deadline and keep waiting for the remainder of
             * its budget -- pr==0 here can be a real (possibly
             * timer-clamped, shorter-than-overall-deadline) timeout, so
             * looping is what lets a timer fire precisely instead of this
             * call returning early every time one is merely pending. */
            if (timeout_ms == 0) {
                return 0;
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
