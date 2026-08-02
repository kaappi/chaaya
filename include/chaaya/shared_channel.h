#ifndef CHAAYA_SHARED_CHANNEL_H
#define CHAAYA_SHARED_CHANNEL_H

#include "chaaya/gc.h"
#include "chaaya/fiber.h"
#include "chaaya/reactor.h"

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bound on concurrently-parked-thread waiters per direction, mirroring
 * CH_CHANNEL_WAITER_MAX for the local (single-thread) channel. */
#define CH_SHARED_CHANNEL_WAITER_MAX 64

typedef struct ChEnvelope {
    ChGC heap; /* private mini-heap holding payload copy when !is_immediate */
    ChValue value;
    int is_immediate;
    int alive;
} ChEnvelope;

typedef struct ChSharedChannel {
    pthread_mutex_t lock;
    size_t capacity; /* 0 = unbounded or rendezvous when rendezvous != 0 */
    uint8_t closed;
    uint8_t rendezvous;
    ChEnvelope **items;
    size_t count;
    size_t head;
    size_t tail;
    size_t storage_cap;
    size_t rv_demand; /* rendezvous: receivers committed to wait */
    int refcount;
    pthread_cond_t send_cv;
    pthread_cond_t recv_cv;
    /* Cross-thread wakeup rings (KEP-0002 §5/§7 shape): each entry is a
     * *retained* reference to some parked thread's reactor notifier.
     * Registered by try_send/try_recv's would-block path, rung (notified
     * and released) on the state transition that side is waiting for. */
    ChThreadNotifier *send_waiters[CH_SHARED_CHANNEL_WAITER_MAX];
    size_t send_waiter_count;
    ChThreadNotifier *recv_waiters[CH_SHARED_CHANNEL_WAITER_MAX];
    size_t recv_waiter_count;
} ChSharedChannel;

/* deepCopy payload into a private envelope heap (or inline immediate). */
ChEnvelope *ch_envelope_create(ChGC *src_gc, ChValue payload);
void ch_envelope_destroy(ChEnvelope *e);
ChValue ch_envelope_copy_out(ChGC *dest, ChEnvelope *e);

ChSharedChannel *ch_shared_channel_create(size_t capacity, int rendezvous);
void ch_shared_channel_retain(ChSharedChannel *sc);
void ch_shared_channel_release(ChSharedChannel *sc);

/* Promote local channel in place; drain local queue into envelopes. Returns 0 or -1. */
int ch_channel_promote(ChGC *gc, ChChannel *ch);

/* Allocate a GC channel stub aliasing an existing shared channel (does not retain). */
ChValue ch_shared_channel_alloc_stub(ChGC *gc, ChSharedChannel *sc, size_t capacity,
                                     int rendezvous, uint8_t closed);

/* try_send/try_recv: 0 ok, 1 would block, -1 closed/error (sets dest gc vm
 * error when applicable). `notifier` is optional (NULL registers nothing):
 * when non-NULL and the call would block, it is registered (retained) in
 * the opposite-direction waiter ring so a future send/recv/close on this
 * channel rings it, per the would_park protocol -- see
 * ch_shared_channel_wait_send_ready / wait_recv_ready below. */
int ch_shared_channel_try_send(ChSharedChannel *sc, ChGC *src_gc, ChValue payload,
                               ChThreadNotifier *notifier);
int ch_shared_channel_try_recv(ChSharedChannel *sc, ChGC *dest_gc, ChValue *out,
                               ChThreadNotifier *notifier);

/* Block until send/recv completes. Prefers a pthread_cond wait rung by
 * ring(); still bounded by a short timedwait so a missed/racy signal (or a
 * caller with no notifier support) cannot hang forever. Callers that drive
 * a fiber scheduler should use try_send/try_recv plus their own reactor
 * poll instead -- see channel_send_shared_timeout in fiber.c. */
int ch_shared_channel_send(ChSharedChannel *sc, ChGC *src_gc, ChValue payload);
int ch_shared_channel_recv(ChSharedChannel *sc, ChGC *dest_gc, ChValue *out);

int ch_shared_channel_close(ChSharedChannel *sc);
int ch_shared_channel_closed(ChSharedChannel *sc);

#ifdef __cplusplus
}
#endif

#endif /* CHAAYA_SHARED_CHANNEL_H */
