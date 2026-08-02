#ifndef CHAAYA_FIBER_INTERNAL_H
#define CHAAYA_FIBER_INTERNAL_H

#include "chaaya/fiber.h"

#include "chaaya/vm.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --- scheduler primitives (fiber.c) --- */
int enqueue_ready_fiber(ChFiberRuntime *runtime, ChValue fiber_v);
int run_next_ready_fiber(ChVM *vm);
int run_scheduler_step_until(ChVM *vm, uint64_t deadline_ms);

/* Poll reactor; wake any fiber payloads into the ready ring. Returns 1 if an
 * event fired, 0 if timed out/idle, -1 on error. */
int poll_reactor_wake(ChVM *vm, uint64_t timeout_ms);

/* Wake all waiters in a waiter list (channel recv/send waiters) back onto
 * the ready ring; clears *count. */
void wake_waiter_list(ChFiberRuntime *runtime, ChValue *waiters, size_t *count);
int add_waiter(ChValue *waiters, size_t *count, ChValue fiber);

/* Park the current fiber (snapshotting VM state) waiting on waiting_on. */
int park_current_fiber(ChVM *vm, ChValue waiting_on, ChFiberParkKind kind);

uint64_t timeout_deadline_ms(double timeout_seconds);
int timeout_expired_ms(uint64_t deadline_ms);

/* --- local (non-shared) channel storage (channel.c) --- */
int channel_grow(ChChannel *channel);
void channel_push(ChChannel *channel, ChValue value);
ChValue channel_pop(ChChannel *channel);
int channel_can_send(ChChannel *channel);

#ifdef __cplusplus
}
#endif

#endif /* CHAAYA_FIBER_INTERNAL_H */
