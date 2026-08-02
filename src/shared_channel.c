#include "chaaya/shared_channel.h"

#include "chaaya/gc_deep_copy.h"
#include "chaaya/vm.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define SC_TRY_OK 0
#define SC_TRY_WOULD_BLOCK 1
#define SC_TRY_ERROR -1

static void sc_set_error(ChGC *gc, const char *msg) {
    if (gc && gc->vm) {
        snprintf(gc->vm->error, sizeof(gc->vm->error), "%s", msg);
    }
}

/* --- Cross-thread waiter rings (would_park protocol) --------------------
 * Caller must hold sc->lock for the *_locked helpers. Registration retains
 * the notifier; ringing notifies then releases it, so ownership always
 * moves from "waiter list entry" to "in-flight notify call" atomically
 * under the lock via the snapshot-and-clear step. */

static void sc_register_waiter_locked(ChThreadNotifier **list, size_t *count,
                                      ChThreadNotifier *notifier) {
    if (!notifier) {
        return;
    }
    for (size_t i = 0; i < *count; i++) {
        if (list[i] == notifier) {
            return; /* dedup: at most one entry per notifier per list */
        }
    }
    if (*count >= CH_SHARED_CHANNEL_WAITER_MAX) {
        /* Best-effort: drop the registration rather than overflow. The
         * caller's own retry loop still makes progress via its poll
         * deadline; it just won't be woken early by this particular
         * event. */
        return;
    }
    ch_thread_notifier_retain(notifier);
    list[(*count)++] = notifier;
}

/* Snapshots and clears a waiter list under the lock, into a caller-owned
 * buffer sized CH_SHARED_CHANNEL_WAITER_MAX. Ownership of each retained
 * reference moves to the snapshot; sc_ring_waiters releases it after
 * notifying. */
static void sc_snapshot_and_clear_locked(ChThreadNotifier **list, size_t *count,
                                         ChThreadNotifier **out, size_t *out_count) {
    for (size_t i = 0; i < *count; i++) {
        out[i] = list[i];
    }
    *out_count = *count;
    *count = 0;
}

/* Rings every notifier in a snapshot taken under the lock, after releasing
 * it -- a live waiter list is never walked unlocked. */
static void sc_ring_waiters(ChThreadNotifier **notifiers, size_t count) {
    for (size_t i = 0; i < count; i++) {
        ch_thread_notifier_notify(notifiers[i]);
        ch_thread_notifier_release(notifiers[i]);
    }
}

/* Releases every remaining registration; used only at channel destruction,
 * when no further send/recv/close can ever ring these lists. */
static void sc_release_waiters_locked(ChThreadNotifier **list, size_t *count) {
    for (size_t i = 0; i < *count; i++) {
        ch_thread_notifier_release(list[i]);
        list[i] = NULL;
    }
    *count = 0;
}

static int value_needs_heap_copy(ChValue v) {
    return ch_is_pointer(v) ? 1 : 0;
}

static void timespec_add_ms(struct timespec *ts, int ms) {
    ts->tv_sec += ms / 1000;
    ts->tv_nsec += (long)(ms % 1000) * 1000000L;
    if (ts->tv_nsec >= 1000000000L) {
        ts->tv_sec++;
        ts->tv_nsec -= 1000000000L;
    }
}

static int sc_ring_grow(ChSharedChannel *sc) {
    size_t old_cap = sc->storage_cap;
    size_t new_cap = old_cap ? old_cap * 2 : 8;
    if (new_cap < old_cap) {
        return -1;
    }
    ChEnvelope **items = (ChEnvelope **)calloc(new_cap, sizeof(ChEnvelope *));
    if (!items) {
        return -1;
    }
    for (size_t i = 0; i < sc->count; i++) {
        size_t old_idx = (sc->head + i) % old_cap;
        items[i] = sc->items[old_idx];
    }
    free(sc->items);
    sc->items = items;
    sc->storage_cap = new_cap;
    sc->head = 0;
    sc->tail = sc->count;
    return 0;
}

static void sc_push_back(ChSharedChannel *sc, ChEnvelope *env) {
    if (sc->count >= sc->storage_cap && sc_ring_grow(sc) != 0) {
        abort();
    }
    sc->items[sc->tail] = env;
    sc->tail = (sc->tail + 1) % sc->storage_cap;
    sc->count++;
}

static ChEnvelope *sc_pop_front(ChSharedChannel *sc) {
    if (sc->count == 0) {
        return NULL;
    }
    ChEnvelope *env = sc->items[sc->head];
    sc->items[sc->head] = NULL;
    sc->head = (sc->head + 1) % sc->storage_cap;
    sc->count--;
    return env;
}

static size_t sc_send_bound(const ChSharedChannel *sc) {
    if (sc->capacity == 0 && sc->rendezvous) {
        return sc->rv_demand;
    }
    return sc->capacity;
}

static int sc_can_send_locked(const ChSharedChannel *sc) {
    if (sc->closed) {
        return 0;
    }
    if (sc->capacity == 0 && !sc->rendezvous) {
        return 1; /* unbounded */
    }
    size_t bound = sc_send_bound(sc);
    return sc->count < bound;
}

ChEnvelope *ch_envelope_create(ChGC *src_gc, ChValue payload) {
    ChEnvelope *e = (ChEnvelope *)calloc(1, sizeof(ChEnvelope));
    if (!e) {
        return NULL;
    }
    e->alive = 1;
    if (!value_needs_heap_copy(payload)) {
        e->is_immediate = 1;
        e->value = payload;
        return e;
    }
    e->is_immediate = 0;
    ch_gc_init(&e->heap);
    e->heap.vm = src_gc ? src_gc->vm : NULL;
    /* Adopt the sending gc's identity: a freshly made (not yet promoted)
     * channel legitimately owned by the sender -- e.g. a fresh reply
     * channel handed off alongside a task, per the (kaappi parallel)
     * pool-submit pattern -- must be promotable while copied into this
     * transient envelope heap. dc_channel_promote_gc only recognizes
     * `dest` itself or `dest`'s parent vm as legal owners; giving the
     * envelope heap the sender's own id makes "owned by the sender" one
     * of those recognized cases instead of a foreign-owner rejection. The
     * id is otherwise unused once ch_envelope_copy_out drains this heap. */
    if (src_gc) {
        e->heap.id = src_gc->id;
    }
    ChValue copy = ch_gc_deep_copy(&e->heap, payload);
    if (copy == CH_UNDEFINED) {
        ch_gc_deinit(&e->heap);
        free(e);
        return NULL;
    }
    e->value = copy;
    return e;
}

void ch_envelope_destroy(ChEnvelope *e) {
    if (!e) {
        return;
    }
    e->alive = 0;
    if (!e->is_immediate) {
        ch_gc_deinit(&e->heap);
    }
    free(e);
}

ChValue ch_envelope_copy_out(ChGC *dest, ChEnvelope *e) {
    if (!e || !e->alive) {
        if (dest) {
            sc_set_error(dest, "shared-channel: invalid envelope");
        }
        return CH_UNDEFINED;
    }
    if (e->is_immediate) {
        return e->value;
    }
    return ch_gc_deep_copy(dest, e->value);
}

ChSharedChannel *ch_shared_channel_create(size_t capacity, int rendezvous) {
    ChSharedChannel *sc = (ChSharedChannel *)calloc(1, sizeof(ChSharedChannel));
    if (!sc) {
        return NULL;
    }
    if (pthread_mutex_init(&sc->lock, NULL) != 0) {
        free(sc);
        return NULL;
    }
    if (pthread_cond_init(&sc->send_cv, NULL) != 0) {
        pthread_mutex_destroy(&sc->lock);
        free(sc);
        return NULL;
    }
    if (pthread_cond_init(&sc->recv_cv, NULL) != 0) {
        pthread_cond_destroy(&sc->send_cv);
        pthread_mutex_destroy(&sc->lock);
        free(sc);
        return NULL;
    }
    sc->capacity = capacity;
    sc->rendezvous = rendezvous ? 1 : 0;
    sc->refcount = 1;
    if (capacity > 0) {
        sc->storage_cap = capacity;
    } else {
        sc->storage_cap = 8;
    }
    sc->items = (ChEnvelope **)calloc(sc->storage_cap, sizeof(ChEnvelope *));
    if (!sc->items) {
        pthread_cond_destroy(&sc->recv_cv);
        pthread_cond_destroy(&sc->send_cv);
        pthread_mutex_destroy(&sc->lock);
        free(sc);
        return NULL;
    }
    return sc;
}

void ch_shared_channel_retain(ChSharedChannel *sc) {
    if (!sc) {
        return;
    }
    pthread_mutex_lock(&sc->lock);
    sc->refcount++;
    pthread_mutex_unlock(&sc->lock);
}

static void sc_destroy(ChSharedChannel *sc) {
    if (!sc) {
        return;
    }
    for (size_t i = 0; i < sc->count; i++) {
        size_t idx = (sc->head + i) % sc->storage_cap;
        ch_envelope_destroy(sc->items[idx]);
        sc->items[idx] = NULL;
    }
    free(sc->items);
    /* No further send/recv/close can ever ring these lists once the last
     * reference is gone: release the retained notifiers directly rather
     * than notifying them (there is nothing left for them to retry). */
    sc_release_waiters_locked(sc->send_waiters, &sc->send_waiter_count);
    sc_release_waiters_locked(sc->recv_waiters, &sc->recv_waiter_count);
    pthread_cond_destroy(&sc->recv_cv);
    pthread_cond_destroy(&sc->send_cv);
    pthread_mutex_destroy(&sc->lock);
    free(sc);
}

void ch_shared_channel_release(ChSharedChannel *sc) {
    if (!sc) {
        return;
    }
    int last = 0;
    pthread_mutex_lock(&sc->lock);
    sc->refcount--;
    if (sc->refcount <= 0) {
        last = 1;
    }
    pthread_mutex_unlock(&sc->lock);
    if (last) {
        sc_destroy(sc);
    }
}

ChValue ch_shared_channel_alloc_stub(ChGC *gc, ChSharedChannel *sc, size_t capacity,
                                     int rendezvous, uint8_t closed) {
    ChValue stub = ch_gc_make_channel(gc, capacity, rendezvous);
    if (!ch_is_channel(stub)) {
        return CH_UNDEFINED;
    }
    ChChannel *ch = ch_as_channel(stub);
    ch->shared = sc;
    ch->closed = closed;
    return stub;
}

int ch_channel_promote(ChGC *gc, ChChannel *ch) {
    if (!gc || !ch) {
        return -1;
    }
    if (ch->shared) {
        return 0;
    }
    if (ch->header.owner != gc->id) {
        sc_set_error(gc, "channel-promote: foreign owner");
        return -1;
    }

    ChSharedChannel *sc = ch_shared_channel_create(ch->capacity, ch->rendezvous ? 1 : 0);
    if (!sc) {
        sc_set_error(gc, "channel-promote: out of memory");
        return -1;
    }
    pthread_mutex_lock(&sc->lock);
    sc->closed = ch->closed;
    pthread_mutex_unlock(&sc->lock);

    ch->shared = sc;

    while (ch->count > 0) {
        ChValue v = ch->items[ch->head];
        ch->items[ch->head] = CH_UNDEFINED;
        ch->head = (ch->head + 1) % ch->storage_cap;
        ch->count--;

        ChEnvelope *env = ch_envelope_create(gc, v);
        if (!env) {
            sc_set_error(gc, "channel-promote: envelope failed");
            return -1;
        }
        pthread_mutex_lock(&sc->lock);
        sc_push_back(sc, env);
        pthread_mutex_unlock(&sc->lock);
    }

    return 0;
}

int ch_shared_channel_try_send(ChSharedChannel *sc, ChGC *src_gc, ChValue payload,
                               ChThreadNotifier *notifier) {
    if (!sc || !src_gc) {
        return SC_TRY_ERROR;
    }
    if (payload == CH_EOF_OBJ) {
        sc_set_error(src_gc, "channel-send: cannot send eof-object");
        return SC_TRY_ERROR;
    }

    pthread_mutex_lock(&sc->lock);
    if (sc->closed) {
        pthread_mutex_unlock(&sc->lock);
        sc_set_error(src_gc, "channel-send: send on closed channel");
        return SC_TRY_ERROR;
    }
    if (!sc_can_send_locked(sc)) {
        sc_register_waiter_locked(sc->send_waiters, &sc->send_waiter_count, notifier);
        pthread_mutex_unlock(&sc->lock);
        return SC_TRY_WOULD_BLOCK;
    }
    pthread_mutex_unlock(&sc->lock);

    ChEnvelope *env = ch_envelope_create(src_gc, payload);
    if (!env) {
        sc_set_error(src_gc, "channel-send: envelope failed");
        return SC_TRY_ERROR;
    }

    ChThreadNotifier *ring_buf[CH_SHARED_CHANNEL_WAITER_MAX];
    size_t ring_count = 0;

    pthread_mutex_lock(&sc->lock);
    int closed_now = sc->closed;
    if (closed_now || !sc_can_send_locked(sc)) {
        if (!closed_now) {
            sc_register_waiter_locked(sc->send_waiters, &sc->send_waiter_count, notifier);
        }
        pthread_mutex_unlock(&sc->lock);
        ch_envelope_destroy(env);
        if (closed_now) {
            sc_set_error(src_gc, "channel-send: send on closed channel");
            return SC_TRY_ERROR;
        }
        return SC_TRY_WOULD_BLOCK;
    }
    sc_push_back(sc, env);
    if (sc->rendezvous && sc->capacity == 0 && sc->rv_demand > 0) {
        sc->rv_demand--;
    }
    sc_snapshot_and_clear_locked(sc->recv_waiters, &sc->recv_waiter_count, ring_buf, &ring_count);
    pthread_cond_broadcast(&sc->recv_cv);
    pthread_mutex_unlock(&sc->lock);
    sc_ring_waiters(ring_buf, ring_count);
    return SC_TRY_OK;
}

int ch_shared_channel_try_recv(ChSharedChannel *sc, ChGC *dest_gc, ChValue *out,
                               ChThreadNotifier *notifier) {
    if (!sc || !dest_gc) {
        return SC_TRY_ERROR;
    }

    ChThreadNotifier *ring_buf[CH_SHARED_CHANNEL_WAITER_MAX];
    size_t ring_count = 0;

    pthread_mutex_lock(&sc->lock);
    if (sc->count == 0) {
        if (sc->closed) {
            pthread_mutex_unlock(&sc->lock);
            if (out) {
                *out = CH_EOF_OBJ;
            }
            return SC_TRY_OK;
        }
        sc_register_waiter_locked(sc->recv_waiters, &sc->recv_waiter_count, notifier);
        pthread_mutex_unlock(&sc->lock);
        return SC_TRY_WOULD_BLOCK;
    }
    ChEnvelope *env = sc_pop_front(sc);
    sc_snapshot_and_clear_locked(sc->send_waiters, &sc->send_waiter_count, ring_buf, &ring_count);
    pthread_cond_broadcast(&sc->send_cv);
    pthread_mutex_unlock(&sc->lock);
    sc_ring_waiters(ring_buf, ring_count);

    ChValue v = ch_envelope_copy_out(dest_gc, env);
    ch_envelope_destroy(env);
    if (v == CH_UNDEFINED && dest_gc->vm && dest_gc->vm->error[0]) {
        return SC_TRY_ERROR;
    }
    if (out) {
        *out = v;
    }
    return SC_TRY_OK;
}

static int sc_cond_wait_ms(pthread_cond_t *cv, pthread_mutex_t *mu, int ms) {
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
        clock_gettime(CLOCK_MONOTONIC, &ts);
    }
    timespec_add_ms(&ts, ms);
    int rc = pthread_cond_timedwait(cv, mu, &ts);
    if (rc == ETIMEDOUT) {
        return 1;
    }
    return 0;
}

/* Blocking fallback for real OS threads that have no fiber runtime (and
 * thus no reactor to hand a notifier to): waits on the pthread_cond,
 * rung by every try_send/try_recv/close that changes the state this side
 * is waiting for, bounded by a short timedwait in case a signal is missed
 * (e.g. a spurious wakeup racing a concurrent waiter-ring registration). */
int ch_shared_channel_send(ChSharedChannel *sc, ChGC *src_gc, ChValue payload) {
    for (;;) {
        int rc = ch_shared_channel_try_send(sc, src_gc, payload, NULL);
        if (rc == SC_TRY_OK) {
            return 0;
        }
        if (rc == SC_TRY_ERROR) {
            return -1;
        }
        pthread_mutex_lock(&sc->lock);
        if (sc->closed) {
            pthread_mutex_unlock(&sc->lock);
            sc_set_error(src_gc, "channel-send: send on closed channel");
            return -1;
        }
        (void)sc_cond_wait_ms(&sc->send_cv, &sc->lock, 1);
        pthread_mutex_unlock(&sc->lock);
    }
}

int ch_shared_channel_recv(ChSharedChannel *sc, ChGC *dest_gc, ChValue *out) {
    int holding_rv = 0;
    for (;;) {
        int rc = ch_shared_channel_try_recv(sc, dest_gc, out, NULL);
        if (rc == SC_TRY_OK) {
            if (holding_rv) {
                pthread_mutex_lock(&sc->lock);
                if (sc->rv_demand > 0) {
                    sc->rv_demand--;
                }
                pthread_mutex_unlock(&sc->lock);
            }
            return 0;
        }
        if (rc == SC_TRY_ERROR) {
            if (holding_rv) {
                pthread_mutex_lock(&sc->lock);
                if (sc->rv_demand > 0) {
                    sc->rv_demand--;
                }
                pthread_mutex_unlock(&sc->lock);
            }
            return -1;
        }

        pthread_mutex_lock(&sc->lock);
        if (sc->count > 0) {
            pthread_mutex_unlock(&sc->lock);
            continue;
        }
        if (sc->closed) {
            pthread_mutex_unlock(&sc->lock);
            if (out) {
                *out = CH_EOF_OBJ;
            }
            return 0;
        }
        if (sc->rendezvous && sc->capacity == 0 && !holding_rv) {
            sc->rv_demand++;
            holding_rv = 1;
            pthread_cond_broadcast(&sc->send_cv);
        }
        (void)sc_cond_wait_ms(&sc->recv_cv, &sc->lock, 1);
        pthread_mutex_unlock(&sc->lock);
    }
}

int ch_shared_channel_close(ChSharedChannel *sc) {
    if (!sc) {
        return -1;
    }
    ChThreadNotifier *ring_buf[2 * CH_SHARED_CHANNEL_WAITER_MAX];
    size_t ring_count = 0;

    pthread_mutex_lock(&sc->lock);
    sc->closed = 1;
    pthread_cond_broadcast(&sc->send_cv);
    pthread_cond_broadcast(&sc->recv_cv);
    sc_snapshot_and_clear_locked(sc->send_waiters, &sc->send_waiter_count, ring_buf, &ring_count);
    size_t recv_count = 0;
    sc_snapshot_and_clear_locked(sc->recv_waiters, &sc->recv_waiter_count, ring_buf + ring_count,
                                 &recv_count);
    ring_count += recv_count;
    pthread_mutex_unlock(&sc->lock);
    sc_ring_waiters(ring_buf, ring_count);
    return 0;
}

int ch_shared_channel_closed(ChSharedChannel *sc) {
    if (!sc) {
        return -1;
    }
    pthread_mutex_lock(&sc->lock);
    int closed = sc->closed ? 1 : 0;
    pthread_mutex_unlock(&sc->lock);
    return closed;
}
