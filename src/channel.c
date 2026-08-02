#include "chaaya/fiber.h"

#include "fiber_internal.h"

#include "chaaya/shared_channel.h"
#include "chaaya/vm.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

int channel_grow(ChChannel *channel) {
    if (!channel) {
        return -1;
    }
    size_t old_cap = channel->storage_cap;
    size_t new_cap = old_cap * 2;
    if (old_cap == 0 || new_cap < old_cap) {
        return -1;
    }
    ChValue *new_items = (ChValue *)calloc(new_cap, sizeof(ChValue));
    if (!new_items) {
        return -1;
    }
    for (size_t i = 0; i < new_cap; i++) {
        new_items[i] = CH_UNDEFINED;
    }
    for (size_t i = 0; i < channel->count; i++) {
        size_t old_idx = (channel->head + i) % old_cap;
        new_items[i] = channel->items[old_idx];
    }
    free(channel->items);
    channel->items = new_items;
    channel->storage_cap = new_cap;
    channel->head = 0;
    channel->tail = channel->count;
    return 0;
}

void channel_push(ChChannel *channel, ChValue value) {
    channel->items[channel->tail] = value;
    channel->tail = (channel->tail + 1) % channel->storage_cap;
    channel->count++;
}

ChValue channel_pop(ChChannel *channel) {
    ChValue value = channel->items[channel->head];
    channel->items[channel->head] = CH_UNDEFINED;
    channel->head = (channel->head + 1) % channel->storage_cap;
    channel->count--;
    return value;
}

int channel_can_send(ChChannel *channel) {
    if (channel->closed) {
        return 0;
    }
    if (channel->rendezvous) {
        /* Rendezvous send waits until a receiver is parked. */
        return channel->recv_waiter_count > 0;
    }
    if (channel->capacity == 0) {
        return 1; /* unbounded */
    }
    return channel->count < channel->capacity;
}

static int channel_check_owner(ChVM *vm, ChValue channel_v, const char *who) {
    if (!ch_is_channel(channel_v)) {
        snprintf(vm->error, sizeof(vm->error), "%s: expected channel", who);
        return -1;
    }
    ChChannel *channel = ch_as_channel(channel_v);
    if (channel->shared != NULL) {
        return 0;
    }
    if (ch_to_object(channel_v)->owner != vm->gc.id) {
        snprintf(vm->error, sizeof(vm->error),
                 "%s: channel belongs to another OS thread", who);
        return -1;
    }
    return 0;
}

/* Bound on the reactor poll issued per would_park iteration of an untimed
 * shared-channel wait, in milliseconds. Cross-thread progress wakes the
 * poll immediately via the registered ChThreadNotifier (see
 * ch_shared_channel_try_send/try_recv); this bound only sets how often a
 * *local* sibling fiber unrelated to this channel gets a chance to run
 * while we wait, and how quickly CH_SHARED_CHANNEL_MAX_IDLE_POLLS'
 * no-progress ceiling below is reached. */
#define CH_SHARED_CHANNEL_IDLE_POLL_MS 50
/* Ceiling on consecutive idle (no local fiber ran, poll timed out) rounds
 * before an untimed shared-channel wait gives up as a deadlock -- roughly
 * CH_SHARED_CHANNEL_IDLE_POLL_MS * this many milliseconds of genuinely no
 * progress from any thread. */
#define CH_SHARED_CHANNEL_MAX_IDLE_POLLS 400

/* would_park retry shape (KEP-0002 §5/§7, adapted): try the op; on a
 * genuine would-block, drive local siblings once (a local fiber may free
 * the very slot we're waiting on), then block this C frame in the
 * reactor's poll -- woken either by a cross-thread ch_thread_notifier_notify
 * (rung by the remote try_send/try_recv/close that observes our
 * registration) or by the bounded fallback above. Always retries through
 * the real try_send/try_recv on wake, never a bare peek: the drive step can
 * consume this fiber's one-shot waiter registration via a sibling
 * operation, so re-calling try_* is what re-registers if still blocked
 * (#1489 shape -- see channel_send_shared_timeout's fuller comment). */
static int channel_send_shared(ChVM *vm, ChChannel *channel, ChValue value) {
    ChSharedChannel *sc = (ChSharedChannel *)channel->shared;
    if (!vm->fiber_runtime || !ch_is_fiber(vm->fiber_runtime->current)) {
        return ch_shared_channel_send(sc, &vm->gc, value);
    }
    ChFiberRuntime *rt = vm->fiber_runtime;
    ChThreadNotifier *notifier = ch_reactor_notify_handle(&rt->reactor);
    int result;
    size_t idle = 0;
    for (;;) {
        int rc = ch_shared_channel_try_send(sc, &vm->gc, value, notifier);
        if (rc == 0) {
            result = 0;
            break;
        }
        if (rc < 0) {
            if (vm->error[0] == '\0') {
                snprintf(vm->error, sizeof(vm->error), "channel-send: shared send failed");
            }
            result = -1;
            break;
        }
        (void)poll_reactor_wake(vm, 0);
        int ran = run_next_ready_fiber(vm);
        if (ran < 0) {
            result = -1;
            break;
        }
        if (ran > 0) {
            idle = 0;
            continue;
        }
        (void)poll_reactor_wake(vm, CH_SHARED_CHANNEL_IDLE_POLL_MS);
        idle++;
        if (idle > CH_SHARED_CHANNEL_MAX_IDLE_POLLS) {
            snprintf(vm->error, sizeof(vm->error),
                     "channel-send!: shared channel made no progress");
            result = -1;
            break;
        }
    }
    ch_thread_notifier_release(notifier);
    return result;
}

static int channel_recv_shared(ChVM *vm, ChChannel *channel, ChValue *out_value) {
    ChSharedChannel *sc = (ChSharedChannel *)channel->shared;
    if (!vm->fiber_runtime || !ch_is_fiber(vm->fiber_runtime->current)) {
        return ch_shared_channel_recv(sc, &vm->gc, out_value);
    }
    ChFiberRuntime *rt = vm->fiber_runtime;
    ChThreadNotifier *notifier = ch_reactor_notify_handle(&rt->reactor);
    int result;
    size_t idle = 0;
    for (;;) {
        int rc = ch_shared_channel_try_recv(sc, &vm->gc, out_value, notifier);
        if (rc == 0) {
            result = 0;
            break;
        }
        if (rc < 0) {
            if (vm->error[0] == '\0') {
                snprintf(vm->error, sizeof(vm->error), "channel-recv: shared receive failed");
            }
            result = -1;
            break;
        }
        (void)poll_reactor_wake(vm, 0);
        int ran = run_next_ready_fiber(vm);
        if (ran < 0) {
            result = -1;
            break;
        }
        if (ran > 0) {
            idle = 0;
            continue;
        }
        (void)poll_reactor_wake(vm, CH_SHARED_CHANNEL_IDLE_POLL_MS);
        idle++;
        if (idle > CH_SHARED_CHANNEL_MAX_IDLE_POLLS) {
            snprintf(vm->error, sizeof(vm->error),
                     "channel-recv: shared channel made no progress");
            result = -1;
            break;
        }
    }
    ch_thread_notifier_release(notifier);
    return result;
}

/* Timed counterpart of channel_send_shared's would_park loop. The notifier
 * is obtained once (not re-fetched per iteration) and passed to every
 * try_send call: on a would-block, try_send (re-)registers it in the
 * channel's send_waiters ring (dedup makes repeated registration of the
 * same notifier a no-op), so a remote try_recv/close rings it and wakes
 * run_scheduler_step_until's reactor poll immediately instead of waiting
 * out the full per-iteration bound. Always retries through a real
 * try_send on wake rather than trusting a bare "is there room now" peek:
 * a sibling's local drive can itself consume this fiber's admission and
 * leave the channel full again before this loop gets back around (#1489
 * shape) -- re-calling try_send is what re-observes and re-registers. */
static int channel_send_shared_timeout(ChVM *vm, ChChannel *channel, ChValue value, uint64_t deadline_ms,
                                       int *timed_out) {
    ChSharedChannel *sc = (ChSharedChannel *)channel->shared;
    ChThreadNotifier *notifier =
        vm->fiber_runtime ? ch_reactor_notify_handle(&vm->fiber_runtime->reactor) : NULL;
    int result;
    for (;;) {
        int rc = ch_shared_channel_try_send(sc, &vm->gc, value, notifier);
        if (rc == 0) {
            result = 0;
            break;
        }
        if (rc < 0) {
            if (vm->error[0] == '\0') {
                snprintf(vm->error, sizeof(vm->error), "channel-send: shared send failed");
            }
            result = -1;
            break;
        }
        if (timeout_expired_ms(deadline_ms)) {
            if (timed_out) {
                *timed_out = 1;
            }
            snprintf(vm->error, sizeof(vm->error), "channel-send: timed out");
            result = -1;
            break;
        }
        int ran = run_scheduler_step_until(vm, deadline_ms);
        if (ran < 0) {
            result = -1;
            break;
        }
    }
    ch_thread_notifier_release(notifier);
    return result;
}

/* Timed counterpart of channel_recv_shared's would_park loop; see
 * channel_send_shared_timeout's comment for the would_park/notifier/retry
 * shape, mirrored here on the receive side. */
static int channel_recv_shared_timeout(ChVM *vm, ChChannel *channel, ChValue *out_value,
                                       uint64_t deadline_ms, int *timed_out) {
    ChSharedChannel *sc = (ChSharedChannel *)channel->shared;
    ChThreadNotifier *notifier =
        vm->fiber_runtime ? ch_reactor_notify_handle(&vm->fiber_runtime->reactor) : NULL;
    int result;
    for (;;) {
        int rc = ch_shared_channel_try_recv(sc, &vm->gc, out_value, notifier);
        if (rc == 0) {
            result = 0;
            break;
        }
        if (rc < 0) {
            if (vm->error[0] == '\0') {
                snprintf(vm->error, sizeof(vm->error), "channel-recv: shared receive failed");
            }
            result = -1;
            break;
        }
        if (timeout_expired_ms(deadline_ms)) {
            if (timed_out) {
                *timed_out = 1;
            }
            snprintf(vm->error, sizeof(vm->error), "channel-receive: timed out");
            result = -1;
            break;
        }
        int ran = run_scheduler_step_until(vm, deadline_ms);
        if (ran < 0) {
            result = -1;
            break;
        }
    }
    ch_thread_notifier_release(notifier);
    return result;
}

int ch_channel_send(ChVM *vm, ChValue channel_v, ChValue value) {
    if (channel_check_owner(vm, channel_v, "channel-send!") != 0) {
        return -1;
    }
    if (value == CH_EOF_OBJ) {
        snprintf(vm->error, sizeof(vm->error),
                 "channel-send: cannot send an eof-object on a channel; use "
                 "channel-close! to end the stream");
        return -1;
    }
    ChChannel *channel = ch_as_channel(channel_v);
    if (channel->shared) {
        return channel_send_shared(vm, channel, value);
    }
    if (channel->closed) {
        snprintf(vm->error, sizeof(vm->error), "channel-send: send on closed channel");
        return -1;
    }

    size_t spins = 0;
    while (!channel_can_send(channel) ||
           (channel->capacity > 0 && !channel->rendezvous &&
            channel->count >= channel->capacity)) {
        if (channel->closed) {
            snprintf(vm->error, sizeof(vm->error), "channel-send: send on closed channel");
            return -1;
        }
        /* Prefer park when running inside a fiber. */
        if (vm->fiber_runtime && ch_is_fiber(vm->fiber_runtime->current)) {
            ChFiber *cur = ch_as_fiber(vm->fiber_runtime->current);
            cur->park_payload = value;
            ch_gc_write_barrier(&vm->gc, &cur->header, value);
            if (add_waiter(channel->send_waiters, &channel->send_waiter_count,
                           vm->fiber_runtime->current) != 0) {
                snprintf(vm->error, sizeof(vm->error), "channel-send!: waiter queue full");
                return -1;
            }
            if (park_current_fiber(vm, channel_v, CH_FIBER_PARK_SEND) != 0) {
                return -1;
            }
            return 0; /* parked; VM will see fiber_parked */
        }
        (void)poll_reactor_wake(vm, 0);
        int ran = run_next_ready_fiber(vm);
        if (ran < 0) {
            return -1;
        }
        if (ran == 0) {
            snprintf(vm->error, sizeof(vm->error),
                     "channel-send!: channel full and no runnable fibers");
            return -1;
        }
        spins++;
        if (spins > CH_FIBER_READY_MAX * 8) {
            snprintf(vm->error, sizeof(vm->error),
                     "channel-send!: scheduler made no channel progress");
            return -1;
        }
    }
    if (channel->closed) {
        snprintf(vm->error, sizeof(vm->error), "channel-send: send on closed channel");
        return -1;
    }
    if (channel->capacity == 0 && !channel->rendezvous &&
        channel->count >= channel->storage_cap) {
        if (channel_grow(channel) != 0) {
            snprintf(vm->error, sizeof(vm->error), "channel-send!: out of memory");
            return -1;
        }
    }
    if (channel->rendezvous && channel->count >= channel->storage_cap) {
        if (channel_grow(channel) != 0) {
            snprintf(vm->error, sizeof(vm->error), "channel-send!: out of memory");
            return -1;
        }
    }
    channel_push(channel, value);
    ch_gc_write_barrier(&vm->gc, &channel->header, value);
    wake_waiter_list(vm->fiber_runtime, channel->recv_waiters, &channel->recv_waiter_count);
    return 0;
}

int ch_channel_send_timeout(ChVM *vm, ChValue channel_v, ChValue value, double timeout_seconds,
                            int *timed_out) {
    if (timed_out) {
        *timed_out = 0;
    }
    if (timeout_seconds < 0.0) {
        return ch_channel_send(vm, channel_v, value);
    }
    if (channel_check_owner(vm, channel_v, "channel-send!") != 0) {
        return -1;
    }
    if (value == CH_EOF_OBJ) {
        snprintf(vm->error, sizeof(vm->error),
                 "channel-send: cannot send an eof-object on a channel; use "
                 "channel-close! to end the stream");
        return -1;
    }
    ChChannel *channel = ch_as_channel(channel_v);
    uint64_t deadline_ms = timeout_deadline_ms(timeout_seconds);
    if (channel->shared) {
        return channel_send_shared_timeout(vm, channel, value, deadline_ms, timed_out);
    }

    for (;;) {
        if (channel->closed) {
            snprintf(vm->error, sizeof(vm->error), "channel-send: send on closed channel");
            return -1;
        }
        if (channel_can_send(channel) &&
            !(channel->capacity > 0 && !channel->rendezvous && channel->count >= channel->capacity)) {
            if (channel->capacity == 0 && !channel->rendezvous &&
                channel->count >= channel->storage_cap) {
                if (channel_grow(channel) != 0) {
                    snprintf(vm->error, sizeof(vm->error), "channel-send!: out of memory");
                    return -1;
                }
            }
            if (channel->rendezvous && channel->count >= channel->storage_cap) {
                if (channel_grow(channel) != 0) {
                    snprintf(vm->error, sizeof(vm->error), "channel-send!: out of memory");
                    return -1;
                }
            }
            channel_push(channel, value);
            ch_gc_write_barrier(&vm->gc, &channel->header, value);
            wake_waiter_list(vm->fiber_runtime, channel->recv_waiters, &channel->recv_waiter_count);
            return 0;
        }
        if (timeout_expired_ms(deadline_ms)) {
            if (timed_out) {
                *timed_out = 1;
            }
            snprintf(vm->error, sizeof(vm->error), "channel-send: timed out");
            return -1;
        }
        int ran = run_scheduler_step_until(vm, deadline_ms);
        if (ran < 0) {
            return -1;
        }
    }
}

int ch_channel_recv(ChVM *vm, ChValue channel_v, ChValue *out_value) {
    if (channel_check_owner(vm, channel_v, "channel-recv") != 0) {
        return -1;
    }
    ChChannel *channel = ch_as_channel(channel_v);
    if (channel->shared) {
        return channel_recv_shared(vm, channel, out_value);
    }
    size_t spins = 0;
    while (channel->count == 0) {
        if (channel->closed) {
            if (out_value) {
                *out_value = CH_EOF_OBJ;
            }
            return 0;
        }
        if (vm->fiber_runtime && ch_is_fiber(vm->fiber_runtime->current)) {
            if (add_waiter(channel->recv_waiters, &channel->recv_waiter_count,
                           vm->fiber_runtime->current) != 0) {
                snprintf(vm->error, sizeof(vm->error), "channel-recv: waiter queue full");
                return -1;
            }
            if (park_current_fiber(vm, channel_v, CH_FIBER_PARK_RECV) != 0) {
                return -1;
            }
            return 0; /* fiber_parked set */
        }
        (void)poll_reactor_wake(vm, 0);
        int ran = run_next_ready_fiber(vm);
        if (ran < 0) {
            return -1;
        }
        if (ran == 0) {
            snprintf(vm->error, sizeof(vm->error),
                     "channel-recv: channel empty and no runnable fibers");
            return -1;
        }
        spins++;
        if (spins > CH_FIBER_READY_MAX * 8) {
            snprintf(vm->error, sizeof(vm->error),
                     "channel-recv: scheduler made no channel progress");
            return -1;
        }
    }
    if (out_value) {
        *out_value = channel_pop(channel);
    } else {
        (void)channel_pop(channel);
    }
    wake_waiter_list(vm->fiber_runtime, channel->send_waiters, &channel->send_waiter_count);
    return 0;
}

int ch_channel_recv_timeout(ChVM *vm, ChValue channel_v, double timeout_seconds, ChValue *out_value,
                            int *timed_out) {
    if (timed_out) {
        *timed_out = 0;
    }
    if (timeout_seconds < 0.0) {
        return ch_channel_recv(vm, channel_v, out_value);
    }
    if (channel_check_owner(vm, channel_v, "channel-recv") != 0) {
        return -1;
    }
    ChChannel *channel = ch_as_channel(channel_v);
    uint64_t deadline_ms = timeout_deadline_ms(timeout_seconds);
    if (channel->shared) {
        return channel_recv_shared_timeout(vm, channel, out_value, deadline_ms, timed_out);
    }

    for (;;) {
        if (channel->count > 0) {
            if (out_value) {
                *out_value = channel_pop(channel);
            } else {
                (void)channel_pop(channel);
            }
            wake_waiter_list(vm->fiber_runtime, channel->send_waiters, &channel->send_waiter_count);
            return 0;
        }
        if (channel->closed) {
            if (out_value) {
                *out_value = CH_EOF_OBJ;
            }
            return 0;
        }
        if (timeout_expired_ms(deadline_ms)) {
            if (timed_out) {
                *timed_out = 1;
            }
            snprintf(vm->error, sizeof(vm->error), "channel-receive: timed out");
            return -1;
        }
        int ran = run_scheduler_step_until(vm, deadline_ms);
        if (ran < 0) {
            return -1;
        }
    }
}

int ch_channel_close(ChVM *vm, ChValue channel_v) {
    if (channel_check_owner(vm, channel_v, "channel-close!") != 0) {
        return -1;
    }
    ChChannel *channel = ch_as_channel(channel_v);
    if (channel->shared) {
        ChSharedChannel *sc = (ChSharedChannel *)channel->shared;
        if (ch_shared_channel_close(sc) != 0) {
            return -1;
        }
        channel->closed = 1;
        return 0;
    }
    if (channel->closed) {
        return 0; /* idempotent */
    }
    channel->closed = 1;
    wake_waiter_list(vm->fiber_runtime, channel->recv_waiters, &channel->recv_waiter_count);
    wake_waiter_list(vm->fiber_runtime, channel->send_waiters, &channel->send_waiter_count);
    return 0;
}

int ch_channel_closed(ChVM *vm, ChValue channel_v, int *out_closed) {
    if (channel_check_owner(vm, channel_v, "channel-closed?") != 0) {
        return -1;
    }
    if (out_closed) {
        ChChannel *channel = ch_as_channel(channel_v);
        if (channel->shared) {
            *out_closed = ch_shared_channel_closed((ChSharedChannel *)channel->shared);
        } else {
            *out_closed = channel->closed ? 1 : 0;
        }
    }
    return 0;
}
