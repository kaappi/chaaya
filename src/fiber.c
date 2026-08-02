#include "chaaya/fiber.h"

#include "fiber_internal.h"

#include "chaaya/shared_channel.h"
#include "chaaya/vm.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void *alloc_or_abort(size_t n, size_t size) {
    if (n == 0) {
        return NULL;
    }
    void *p = calloc(n, size);
    if (!p) {
        abort();
    }
    return p;
}

static void free_snapshot(ChFiberSnapshot *snap) {
    if (!snap || !snap->valid) {
        return;
    }
    free(snap->registers);
    free(snap->frames);
    free(snap->winds);
    free(snap->handlers);
    free(snap->parameter_bindings);
    free(snap->open_uvs);
    memset(snap, 0, sizeof(*snap));
}

int enqueue_ready_fiber(ChFiberRuntime *runtime, ChValue fiber_v) {
    if (!runtime || runtime->ready_count >= CH_FIBER_READY_MAX) {
        return -1;
    }
    if (!ch_is_fiber(fiber_v)) {
        return -1;
    }
    ChFiber *fiber = ch_as_fiber(fiber_v);
    if (fiber->queued) {
        return 0;
    }
    size_t tail = (runtime->ready_head + runtime->ready_count) % CH_FIBER_READY_MAX;
    runtime->ready[tail] = fiber_v;
    runtime->ready_count++;
    fiber->queued = 1;
    return 0;
}

static int dequeue_ready_fiber(ChFiberRuntime *runtime, ChValue *out_fiber) {
    if (!runtime || runtime->ready_count == 0) {
        return 0;
    }
    *out_fiber = runtime->ready[runtime->ready_head];
    runtime->ready[runtime->ready_head] = CH_NIL;
    runtime->ready_head = (runtime->ready_head + 1) % CH_FIBER_READY_MAX;
    runtime->ready_count--;
    if (ch_is_fiber(*out_fiber)) {
        ch_as_fiber(*out_fiber)->queued = 0;
    }
    return 1;
}

void wake_waiter_list(ChFiberRuntime *runtime, ChValue *waiters, size_t *count) {
    for (size_t i = 0; i < *count; i++) {
        ChValue fv = waiters[i];
        waiters[i] = CH_NIL;
        if (!ch_is_fiber(fv)) {
            continue;
        }
        ChFiber *f = ch_as_fiber(fv);
        if (f->state == CH_FIBER_WAITING || f->state == CH_FIBER_IO_WAITING) {
            /* Keep waiting_on until resume consumes park_kind / channel. */
            f->state = CH_FIBER_READY;
            (void)enqueue_ready_fiber(runtime, fv);
        }
    }
    *count = 0;
}

int add_waiter(ChValue *waiters, size_t *count, ChValue fiber) {
    if (*count >= CH_CHANNEL_WAITER_MAX) {
        return -1;
    }
    waiters[(*count)++] = fiber;
    return 0;
}

uint64_t timeout_deadline_ms(double timeout_seconds) {
    if (timeout_seconds <= 0.0) {
        return ch_reactor_now_ms();
    }
    double ms_f = timeout_seconds * 1000.0;
    if (ms_f >= (double)UINT64_MAX) {
        return UINT64_MAX;
    }
    uint64_t delta = (uint64_t)(ms_f + 0.5);
    uint64_t now = ch_reactor_now_ms();
    if (UINT64_MAX - now < delta) {
        return UINT64_MAX;
    }
    return now + delta;
}

int timeout_expired_ms(uint64_t deadline_ms) {
    return ch_reactor_now_ms() >= deadline_ms;
}

/* O(1) via the reactor's timer heap root instead of scanning every slot. */
static uint64_t next_reactor_wait_ms(const ChReactor *reactor) {
    if (!reactor) {
        return 0;
    }
    uint64_t due_ns = ch_reactor_earliest_due_ns(reactor);
    if (due_ns == UINT64_MAX) {
        /* No timers pending: fall back to a short poll interval when fd
         * waiters exist (their readiness isn't known without polling). */
        return reactor->fd_count > 0 ? 50 : 0;
    }
    uint64_t now_ns = ch_reactor_now_ns();
    if (due_ns <= now_ns) {
        return 0;
    }
    return (due_ns - now_ns) / 1000000ULL;
}

int poll_reactor_wake(ChVM *vm, uint64_t timeout_ms) {
    if (!vm || !vm->fiber_runtime) {
        return 0;
    }
    ChFiberRuntime *rt = vm->fiber_runtime;
    ChValue payload = CH_UNDEFINED;
    int r = ch_reactor_poll(&rt->reactor, timeout_ms, &payload);
    if (r <= 0) {
        return r;
    }
    if (ch_is_fiber(payload)) {
        ChFiber *f = ch_as_fiber(payload);
        if (f->state == CH_FIBER_WAITING || f->state == CH_FIBER_IO_WAITING) {
            f->state = CH_FIBER_READY;
            (void)enqueue_ready_fiber(rt, payload);
        }
    }
    return 1;
}

void ch_fiber_runtime_init(ChFiberRuntime *runtime) {
    if (!runtime) {
        return;
    }
    memset(runtime, 0, sizeof(*runtime));
    runtime->current = CH_NIL;
    runtime->next_id = 1;
    for (size_t i = 0; i < CH_FIBER_READY_MAX; i++) {
        runtime->ready[i] = CH_NIL;
    }
    ch_reactor_init(&runtime->reactor);
}

void ch_fiber_runtime_deinit(ChFiberRuntime *runtime) {
    if (!runtime) {
        return;
    }
    ch_reactor_deinit(&runtime->reactor);
    for (size_t i = 0; i < CH_FIBER_READY_MAX; i++) {
        runtime->ready[i] = CH_NIL;
    }
    runtime->ready_head = 0;
    runtime->ready_count = 0;
    runtime->current = CH_NIL;
}

size_t ch_fiber_runtime_root_count(const ChFiberRuntime *runtime) {
    if (!runtime) {
        return 0;
    }
    return runtime->ready_count + 1 + runtime->reactor.timer_count +
           runtime->reactor.fd_count;
}

size_t ch_fiber_runtime_push_roots(ChGC *gc, ChFiberRuntime *runtime) {
    if (!gc || !runtime) {
        return 0;
    }
    size_t pushed = 0;
    for (size_t i = 0; i < runtime->ready_count; i++) {
        size_t idx = (runtime->ready_head + i) % CH_FIBER_READY_MAX;
        ch_gc_push(gc, &runtime->ready[idx]);
        pushed++;
    }
    ch_gc_push(gc, &runtime->current);
    pushed++;
    pushed += ch_reactor_push_roots(gc, &runtime->reactor);
    return pushed;
}

ChValue ch_gc_make_fiber(ChGC *gc, uint64_t id, ChValue thunk) {
    ch_gc_push(gc, &thunk);
    ChFiber *fiber = (ChFiber *)ch_gc_alloc(gc, sizeof(ChFiber), CH_TAG_FIBER);
    ch_gc_pop(gc);
    fiber->id = id;
    fiber->state = CH_FIBER_READY;
    fiber->park_kind = CH_FIBER_PARK_NONE;
    fiber->queued = 0;
    fiber->os_state = CH_OS_THREAD_NONE;
    fiber->terminated = 0;
    fiber->thunk = thunk;
    fiber->result = CH_UNDEFINED;
    fiber->error = CH_NIL;
    fiber->waiting_on = CH_NIL;
    fiber->park_payload = CH_UNDEFINED;
    fiber->name = CH_FALSE;
    fiber->specific = CH_FALSE;
    memset(&fiber->snapshot, 0, sizeof(fiber->snapshot));
    fiber->io_fd = -1;
    fiber->io_interest = 0;
    fiber->os_join = NULL;
    return ch_make_pointer(&fiber->header);
}

ChValue ch_gc_make_channel(ChGC *gc, size_t capacity, int rendezvous) {
    ChChannel *channel = (ChChannel *)ch_gc_alloc(gc, sizeof(ChChannel), CH_TAG_CHANNEL);
    channel->capacity = capacity;
    channel->rendezvous = rendezvous ? 1 : 0;
    channel->closed = 0;
    channel->storage_cap = (capacity == 0 && !rendezvous) ? 8 : (capacity == 0 ? 1 : capacity);
    if (channel->storage_cap == 0) {
        channel->storage_cap = 1;
    }
    channel->count = 0;
    channel->head = 0;
    channel->tail = 0;
    channel->recv_waiter_count = 0;
    channel->send_waiter_count = 0;
    channel->shared = NULL;
    channel->items = (ChValue *)calloc(channel->storage_cap, sizeof(ChValue));
    if (!channel->items) {
        abort();
    }
    for (size_t i = 0; i < channel->storage_cap; i++) {
        channel->items[i] = CH_UNDEFINED;
    }
    for (size_t i = 0; i < CH_CHANNEL_WAITER_MAX; i++) {
        channel->recv_waiters[i] = CH_NIL;
        channel->send_waiters[i] = CH_NIL;
    }
    return ch_make_pointer(&channel->header);
}

int ch_fiber_save_snapshot(ChVM *vm, ChFiber *fiber) {
    if (!vm || !fiber) {
        return -1;
    }
    free_snapshot(&fiber->snapshot);
    ChFiberRuntime *rt = vm->fiber_runtime;
    size_t entry_frames = rt ? rt->entry_frames : 0;
    size_t entry_reg = rt ? rt->entry_reg_top : 0;
    if (vm->frame_count < entry_frames || vm->reg_top < entry_reg) {
        return -1;
    }

    /* Store only the fiber's slice, with registers/frames relative to entry. */
    fiber->snapshot.entry_frames = 0;
    fiber->snapshot.entry_reg_top = 0;
    size_t nregs = vm->reg_top - entry_reg;
    size_t nframes = vm->frame_count - entry_frames;
    fiber->snapshot.result_slot =
        (vm->native_result_slot >= entry_reg) ? (vm->native_result_slot - entry_reg)
                                              : 0;
    fiber->snapshot.register_count = nregs;
    fiber->snapshot.registers = (ChValue *)alloc_or_abort(nregs, sizeof(ChValue));
    memcpy(fiber->snapshot.registers, vm->regs + entry_reg, nregs * sizeof(ChValue));

    fiber->snapshot.frame_count = nframes;
    fiber->snapshot.frames =
        (ChSavedFrame *)alloc_or_abort(nframes, sizeof(ChSavedFrame));
    for (size_t i = 0; i < nframes; i++) {
        ChCallFrame *f = &vm->frames[entry_frames + i];
        fiber->snapshot.frames[i].closure = f->closure;
        fiber->snapshot.frames[i].ip_offset = (size_t)(f->ip - f->closure->fn->code);
        fiber->snapshot.frames[i].reg_base = f->reg_base - entry_reg;
        fiber->snapshot.frames[i].num_regs = f->num_regs;
    }

    fiber->snapshot.wind_count = vm->wind_count;
    fiber->snapshot.winds =
        (ChWindRecord *)alloc_or_abort(vm->wind_count, sizeof(ChWindRecord));
    memcpy(fiber->snapshot.winds, vm->wind_stack, vm->wind_count * sizeof(ChWindRecord));

    fiber->snapshot.handler_count = vm->handler_count;
    fiber->snapshot.handlers = (ChExceptionHandler *)alloc_or_abort(
        vm->handler_count, sizeof(ChExceptionHandler));
    memcpy(fiber->snapshot.handlers, vm->handler_stack,
           vm->handler_count * sizeof(ChExceptionHandler));

    fiber->snapshot.parameter_binding_count = vm->parameter_count;
    fiber->snapshot.parameter_bindings = (ChParameterBinding *)alloc_or_abort(
        vm->parameter_count, sizeof(ChParameterBinding));
    memcpy(fiber->snapshot.parameter_bindings, vm->parameter_stack,
           vm->parameter_count * sizeof(ChParameterBinding));

    size_t nuv = 0;
    for (ChUpvalue *uv = vm->open_upvalues; uv; uv = uv->next) {
        size_t idx = (size_t)(uv->location - vm->regs);
        if (idx >= entry_reg) {
            nuv++;
        }
    }
    fiber->snapshot.open_uv_count = nuv;
    fiber->snapshot.open_uvs =
        (ChSavedUpvalue *)alloc_or_abort(nuv, sizeof(ChSavedUpvalue));
    size_t ui = 0;
    for (ChUpvalue *uv = vm->open_upvalues; uv; uv = uv->next) {
        size_t idx = (size_t)(uv->location - vm->regs);
        if (idx < entry_reg) {
            continue;
        }
        fiber->snapshot.open_uvs[ui].uv = uv;
        fiber->snapshot.open_uvs[ui].reg_index = idx - entry_reg;
        ui++;
    }
    fiber->snapshot.valid = 1;
    return 0;
}

int ch_fiber_restore_snapshot(ChVM *vm, ChFiber *fiber, ChValue inject) {
    if (!vm || !fiber || !fiber->snapshot.valid) {
        return -1;
    }
    ChFiberSnapshot *snap = &fiber->snapshot;
    size_t base_reg = vm->reg_top;
    size_t base_frames = vm->frame_count;
    if (base_reg + snap->register_count > CH_VM_MAX_REGS ||
        base_frames + snap->frame_count > CH_VM_MAX_FRAMES) {
        return -1;
    }
    memcpy(vm->regs + base_reg, snap->registers,
           snap->register_count * sizeof(ChValue));
    vm->reg_top = base_reg + snap->register_count;
    for (size_t i = 0; i < snap->frame_count; i++) {
        ChSavedFrame *sf = &snap->frames[i];
        ChCallFrame *f = &vm->frames[base_frames + i];
        f->closure = sf->closure;
        f->ip = sf->closure->fn->code + sf->ip_offset;
        f->reg_base = base_reg + sf->reg_base;
        f->num_regs = sf->num_regs;
    }
    vm->frame_count = base_frames + snap->frame_count;

    /* Replace dynamic stacks with the fiber's saved view. */
    memcpy(vm->handler_stack, snap->handlers,
           snap->handler_count * sizeof(ChExceptionHandler));
    vm->handler_count = snap->handler_count;
    memcpy(vm->parameter_stack, snap->parameter_bindings,
           snap->parameter_binding_count * sizeof(ChParameterBinding));
    vm->parameter_count = snap->parameter_binding_count;
    memcpy(vm->wind_stack, snap->winds, snap->wind_count * sizeof(ChWindRecord));
    vm->wind_count = snap->wind_count;

    for (size_t i = 0; i < snap->open_uv_count; i++) {
        ChUpvalue *uv = snap->open_uvs[i].uv;
        size_t idx = base_reg + snap->open_uvs[i].reg_index;
        if (idx >= vm->reg_top) {
            continue;
        }
        if (uv->is_closed) {
            vm->regs[idx] = uv->closed_value;
        }
        uv->location = &vm->regs[idx];
        uv->is_closed = false;
        uv->next = vm->open_upvalues;
        vm->open_upvalues = uv;
    }

    size_t result_slot = base_reg + snap->result_slot;
    if (result_slot < vm->reg_top) {
        vm->regs[result_slot] = inject;
    }
    vm->result = inject;
    /* Stash resume barrier for run_until / result extraction. */
    if (vm->fiber_runtime) {
        vm->fiber_runtime->entry_frames = base_frames;
        vm->fiber_runtime->entry_reg_top = base_reg;
    }
    free_snapshot(snap);
    return 0;
}

int park_current_fiber(ChVM *vm, ChValue waiting_on, ChFiberParkKind kind) {
    ChFiberRuntime *rt = vm->fiber_runtime;
    if (!rt || !ch_is_fiber(rt->current)) {
        snprintf(vm->error, sizeof(vm->error), "fiber: park outside fiber");
        return -1;
    }
    ChFiber *fiber = ch_as_fiber(rt->current);
    if (ch_fiber_save_snapshot(vm, fiber) != 0) {
        snprintf(vm->error, sizeof(vm->error), "fiber: snapshot failed");
        return -1;
    }
    fiber->state = CH_FIBER_WAITING;
    fiber->park_kind = (uint8_t)kind;
    fiber->waiting_on = waiting_on;
    ch_gc_write_barrier(&vm->gc, &fiber->header, waiting_on);
    vm->fiber_parked = true;
    return 0;
}

static int run_fiber_to_park_or_done(ChVM *vm, ChValue fiber_v) {
    ChFiberRuntime *runtime = vm->fiber_runtime;
    ChFiber *fiber = ch_as_fiber(fiber_v);
    ChValue previous_current = runtime->current;
    runtime->current = fiber_v;
    fiber->state = CH_FIBER_RUNNING;
    vm->fiber_parked = false;

    ChValue result = CH_VOID;
    ChVMStatus st;

    if (fiber->snapshot.valid) {
        ChValue inject = CH_UNDEFINED;
        ChFiberParkKind kind = (ChFiberParkKind)fiber->park_kind;
        ChValue waiting = fiber->waiting_on;
        fiber->waiting_on = CH_NIL;
        fiber->park_kind = CH_FIBER_PARK_NONE;
        if (kind == CH_FIBER_PARK_RECV && ch_is_channel(waiting)) {
            ChChannel *ch = ch_as_channel(waiting);
            if (ch->count > 0) {
                inject = channel_pop(ch);
                wake_waiter_list(runtime, ch->send_waiters, &ch->send_waiter_count);
            } else if (ch->closed) {
                inject = CH_EOF_OBJ;
            } else {
                fiber->waiting_on = waiting;
                fiber->park_kind = (uint8_t)kind;
                fiber->state = CH_FIBER_WAITING;
                if (add_waiter(ch->recv_waiters, &ch->recv_waiter_count, fiber_v) != 0) {
                    runtime->current = previous_current;
                    return -1;
                }
                runtime->current = previous_current;
                return 0;
            }
        } else if (kind == CH_FIBER_PARK_SEND && ch_is_channel(waiting)) {
            ChChannel *ch = ch_as_channel(waiting);
            ChValue payload = fiber->park_payload;
            fiber->park_payload = CH_UNDEFINED;
            if (ch->closed) {
                /* Raise inside the fiber so (guard ...) can catch it. */
                if (ch_fiber_restore_snapshot(vm, fiber, CH_UNDEFINED) != 0) {
                    fiber->state = CH_FIBER_FAILED;
                    runtime->current = previous_current;
                    return -1;
                }
                ChValue msg = ch_gc_make_string_cstr(
                    &vm->gc, "channel-send: send on closed channel");
                ch_gc_push(&vm->gc, &msg);
                ChValue err = ch_gc_make_error_object(&vm->gc, msg, CH_NIL, 0);
                ch_gc_pop(&vm->gc);
                (void)ch_vm_raise(vm, err, 0);
                st = ch_vm_run_fiber_resume(vm, runtime->entry_frames);
                goto after_resume;
            }
            if (!channel_can_send(ch)) {
                fiber->park_payload = payload;
                fiber->waiting_on = waiting;
                fiber->park_kind = (uint8_t)kind;
                fiber->state = CH_FIBER_WAITING;
                if (add_waiter(ch->send_waiters, &ch->send_waiter_count, fiber_v) != 0) {
                    runtime->current = previous_current;
                    return -1;
                }
                runtime->current = previous_current;
                return 0;
            }
            if (ch->capacity == 0 && !ch->rendezvous && ch->count >= ch->storage_cap) {
                if (channel_grow(ch) != 0) {
                    runtime->current = previous_current;
                    return -1;
                }
            }
            channel_push(ch, payload);
            ch_gc_write_barrier(&vm->gc, &ch->header, payload);
            wake_waiter_list(runtime, ch->recv_waiters, &ch->recv_waiter_count);
            inject = CH_VOID;
        } else {
            inject = CH_VOID;
        }

        if (ch_fiber_restore_snapshot(vm, fiber, inject) != 0) {
            fiber->state = CH_FIBER_FAILED;
            fiber->error = ch_gc_make_string_cstr(&vm->gc, "fiber: restore failed");
            runtime->current = previous_current;
            return -1;
        }
        st = ch_vm_run_fiber_resume(vm, runtime->entry_frames);
    after_resume:
        if (st == CH_VM_FIBER_PARKED || vm->fiber_parked) {
            vm->fiber_parked = false;
            vm->frame_count = runtime->entry_frames;
            vm->reg_top = runtime->entry_reg_top;
            runtime->current = previous_current;
            return 0;
        }
        if (st == CH_VM_CONTINUATION_INVOKED) {
            fiber->state = CH_FIBER_FAILED;
            fiber->error = ch_gc_make_string_cstr(
                &vm->gc, "fiber: continuations across scheduler are unsupported");
            ch_gc_write_barrier(&vm->gc, &fiber->header, fiber->error);
            snprintf(vm->error, sizeof(vm->error),
                     "fiber: continuations across scheduler are unsupported");
            runtime->current = previous_current;
            return -1;
        }
        if (st != CH_VM_OK) {
            fiber->state = CH_FIBER_FAILED;
            const char *msg = vm->error[0] ? vm->error : "fiber failed";
            fiber->error = ch_gc_make_string_cstr(&vm->gc, msg);
            ch_gc_write_barrier(&vm->gc, &fiber->header, fiber->error);
            runtime->current = previous_current;
            return -1;
        }
        /* Thunk return lives in the apply base register (entry_reg_top). */
        result = (runtime->entry_reg_top < CH_VM_MAX_REGS)
                     ? vm->regs[runtime->entry_reg_top]
                     : vm->result;
        fiber->state = CH_FIBER_DONE;
        fiber->result = ch_coerce_single(result);
        fiber->error = CH_NIL;
        ch_gc_write_barrier(&vm->gc, &fiber->header, fiber->result);
        free_snapshot(&fiber->snapshot);
        runtime->current = previous_current;
        return 1;
    }

    runtime->entry_frames = vm->frame_count;
    runtime->entry_reg_top = vm->reg_top;
    st = ch_vm_apply(vm, fiber->thunk, NULL, 0, &result);
    if (st == CH_VM_FIBER_PARKED || vm->fiber_parked) {
        vm->fiber_parked = false;
        /* Snapshot already saved; unwind to entry. */
        vm->frame_count = runtime->entry_frames;
        vm->reg_top = runtime->entry_reg_top;
        runtime->current = previous_current;
        return 0;
    }
    if (st == CH_VM_OK) {
        fiber->state = CH_FIBER_DONE;
        fiber->result = ch_coerce_single(result);
        fiber->error = CH_NIL;
        ch_gc_write_barrier(&vm->gc, &fiber->header, fiber->result);
        free_snapshot(&fiber->snapshot);
    } else if (st == CH_VM_CONTINUATION_INVOKED) {
        fiber->state = CH_FIBER_FAILED;
        fiber->result = CH_UNDEFINED;
        fiber->error = ch_gc_make_string_cstr(
            &vm->gc, "fiber: continuations across scheduler are unsupported");
        ch_gc_write_barrier(&vm->gc, &fiber->header, fiber->error);
        snprintf(vm->error, sizeof(vm->error),
                 "fiber: continuations across scheduler are unsupported");
        runtime->current = previous_current;
        return -1;
    } else {
        fiber->state = CH_FIBER_FAILED;
        fiber->result = CH_UNDEFINED;
        const char *msg = vm->error[0] ? vm->error : "fiber failed";
        fiber->error = ch_gc_make_string_cstr(&vm->gc, msg);
        ch_gc_write_barrier(&vm->gc, &fiber->header, fiber->error);
        runtime->current = previous_current;
        return -1;
    }

    runtime->current = previous_current;
    return 1;
}

int run_next_ready_fiber(ChVM *vm) {
    ChFiberRuntime *runtime = vm->fiber_runtime;
    ChValue fiber_v = CH_NIL;
    int has_fiber = dequeue_ready_fiber(runtime, &fiber_v);
    if (has_fiber <= 0) {
        return 0;
    }
    if (!ch_is_fiber(fiber_v)) {
        return 0;
    }
    ChFiber *fiber = ch_as_fiber(fiber_v);
    if (fiber->state != CH_FIBER_READY && !fiber->snapshot.valid) {
        return 0;
    }
    if (fiber->state == CH_FIBER_WAITING && fiber->snapshot.valid) {
        fiber->state = CH_FIBER_READY;
    }
    ch_gc_push(&vm->gc, &fiber_v);
    int rc = run_fiber_to_park_or_done(vm, fiber_v);
    ch_gc_pop(&vm->gc);
    return rc;
}

int ch_fiber_drive(ChVM *vm) {
    if (!vm->fiber_runtime) {
        return 0;
    }
    ChFiberRuntime *rt = vm->fiber_runtime;
    size_t idle = 0;
    for (;;) {
        (void)poll_reactor_wake(vm, 0);
        if (rt->ready_count == 0) {
            /* Nothing runnable and nothing left that could ever wake a
             * fiber (no pending timers, no fd waiters): done, cleanly. */
            if (ch_reactor_is_empty(&rt->reactor)) {
                break;
            }
            uint64_t wait_ms = next_reactor_wait_ms(&rt->reactor);
            (void)poll_reactor_wake(vm, wait_ms == 0 ? 1 : wait_ms);
            if (rt->ready_count == 0) {
                idle++;
                if (idle > CH_FIBER_READY_MAX * 4) {
                    break;
                }
                continue;
            }
        }
        int ran = run_next_ready_fiber(vm);
        if (ran < 0) {
            return -1;
        }
        if (ran == 0) {
            idle++;
            if (idle > CH_FIBER_READY_MAX * 4) {
                break;
            }
        } else {
            idle = 0;
        }
    }
    return 0;
}

int ch_fiber_spawn(ChVM *vm, ChValue thunk, ChValue *out_fiber) {
    if (!ch_is_procedure(thunk)) {
        snprintf(vm->error, sizeof(vm->error), "spawn-fiber: expected procedure");
        return -1;
    }
    if (!vm->fiber_runtime) {
        snprintf(vm->error, sizeof(vm->error), "spawn-fiber: fiber runtime unavailable");
        return -1;
    }
    ChFiberRuntime *runtime = vm->fiber_runtime;
    ChValue fiber = ch_gc_make_fiber(&vm->gc, runtime->next_id++, thunk);
    if (enqueue_ready_fiber(runtime, fiber) != 0) {
        snprintf(vm->error, sizeof(vm->error), "spawn-fiber: run queue full");
        return -1;
    }
    if (out_fiber) {
        *out_fiber = fiber;
    }
    return 0;
}

int ch_fiber_yield(ChVM *vm) {
    if (!vm->fiber_runtime) {
        snprintf(vm->error, sizeof(vm->error), "fiber-yield: fiber runtime unavailable");
        return -1;
    }
    (void)poll_reactor_wake(vm, 0);
    /* Nested cooperative yield: run other ready fibers; current stays on C stack. */
    int ran = run_next_ready_fiber(vm);
    if (ran < 0) {
        return -1;
    }
    return 0;
}

int ch_fiber_join(ChVM *vm, ChValue fiber_v, ChValue *out_result) {
    if (!vm->fiber_runtime) {
        snprintf(vm->error, sizeof(vm->error), "fiber-join: fiber runtime unavailable");
        return -1;
    }
    if (!ch_is_fiber(fiber_v)) {
        snprintf(vm->error, sizeof(vm->error), "fiber-join: expected fiber");
        return -1;
    }

    ch_gc_push(&vm->gc, &fiber_v);
    ChFiber *fiber = ch_as_fiber(fiber_v);
    ChFiberRuntime *rt = vm->fiber_runtime;
    while (fiber->state != CH_FIBER_DONE && fiber->state != CH_FIBER_FAILED) {
        (void)poll_reactor_wake(vm, 0);
        int ran = 0;
        if (rt->ready_count > 0) {
            ran = run_next_ready_fiber(vm);
            if (ran < 0) {
                ch_gc_pop(&vm->gc);
                return -1;
            }
        }
        if (ran == 0 && rt->ready_count == 0) {
            /* Nothing runnable and nothing left that could ever wake the
             * awaited fiber (or any other): this can never make progress. */
            if (ch_reactor_is_empty(&rt->reactor)) {
                snprintf(vm->error, sizeof(vm->error),
                         "fiber-join: deadlock detected joining fiber %llu — no "
                         "runnable fibers and no pending timers or I/O to wake one",
                         (unsigned long long)fiber->id);
                ch_gc_pop(&vm->gc);
                return -1;
            }
            uint64_t wait_ms = next_reactor_wait_ms(&rt->reactor);
            (void)poll_reactor_wake(vm, wait_ms == 0 ? 1 : wait_ms);
        }
    }

    ch_gc_pop(&vm->gc);
    if (fiber->state == CH_FIBER_DONE) {
        if (out_result) {
            *out_result = fiber->result;
        }
        return 0;
    }

    if (ch_is_string(fiber->error)) {
        snprintf(vm->error, sizeof(vm->error), "%s", ch_as_string(fiber->error)->data);
    } else {
        snprintf(vm->error, sizeof(vm->error), "fiber-join: fiber failed");
    }
    return -1;
}

int ch_fiber_join_timeout(ChVM *vm, ChValue fiber_v, double timeout_seconds, ChValue *out_result,
                          int *timed_out) {
    if (timed_out) {
        *timed_out = 0;
    }
    if (timeout_seconds < 0.0) {
        return ch_fiber_join(vm, fiber_v, out_result);
    }
    if (!vm->fiber_runtime) {
        snprintf(vm->error, sizeof(vm->error), "fiber-join: fiber runtime unavailable");
        return -1;
    }
    if (!ch_is_fiber(fiber_v)) {
        snprintf(vm->error, sizeof(vm->error), "fiber-join: expected fiber");
        return -1;
    }

    ch_gc_push(&vm->gc, &fiber_v);
    ChFiber *fiber = ch_as_fiber(fiber_v);
    uint64_t deadline_ms = timeout_deadline_ms(timeout_seconds);
    while (fiber->state != CH_FIBER_DONE && fiber->state != CH_FIBER_FAILED) {
        if (timeout_expired_ms(deadline_ms)) {
            if (timed_out) {
                *timed_out = 1;
            }
            ch_gc_pop(&vm->gc);
            return -1;
        }
        int ran = run_scheduler_step_until(vm, deadline_ms);
        if (ran < 0) {
            ch_gc_pop(&vm->gc);
            return -1;
        }
    }

    ch_gc_pop(&vm->gc);
    if (fiber->state == CH_FIBER_DONE) {
        if (out_result) {
            *out_result = fiber->result;
        }
        return 0;
    }

    if (ch_is_string(fiber->error)) {
        snprintf(vm->error, sizeof(vm->error), "%s", ch_as_string(fiber->error)->data);
    } else {
        snprintf(vm->error, sizeof(vm->error), "fiber-join: fiber failed");
    }
    return -1;
}

int ch_fiber_sleep(ChVM *vm, double seconds) {
    if (!vm || !vm->fiber_runtime) {
        snprintf(vm->error, sizeof(vm->error), "thread-sleep!: fiber runtime unavailable");
        return -1;
    }
    if (seconds <= 0.0) {
        return 0;
    }
    uint64_t delay_ms = (uint64_t)(seconds * 1000.0 + 0.5);
    if (delay_ms == 0) {
        delay_ms = 1;
    }

    ChFiberRuntime *rt = vm->fiber_runtime;
    if (ch_is_fiber(rt->current)) {
        ChValue fiber_v = rt->current;
        uint64_t id = 0;
        if (ch_reactor_schedule(&rt->reactor, delay_ms, fiber_v, &id) != 0) {
            snprintf(vm->error, sizeof(vm->error), "thread-sleep!: timer heap full");
            return -1;
        }
        ChFiber *fiber = ch_as_fiber(fiber_v);
        fiber->io_fd = -1;
        if (park_current_fiber(vm, CH_VOID, CH_FIBER_PARK_SLEEP) != 0) {
            (void)ch_reactor_cancel(&rt->reactor, id);
            return -1;
        }
        return 0;
    }

    /* Outside a fiber: block on the reactor while still driving siblings. */
    ChValue done = CH_TRUE;
    uint64_t id = 0;
    if (ch_reactor_schedule(&rt->reactor, delay_ms, done, &id) != 0) {
        snprintf(vm->error, sizeof(vm->error), "thread-sleep!: timer heap full");
        return -1;
    }
    for (;;) {
        int still = 0;
        for (size_t i = 0; i < CH_REACTOR_MAX_TIMERS; i++) {
            if (rt->reactor.timers[i].active && rt->reactor.timers[i].id == id) {
                still = 1;
                break;
            }
        }
        if (!still) {
            return 0;
        }
        uint64_t wait_ms = next_reactor_wait_ms(&rt->reactor);
        ChValue payload = CH_UNDEFINED;
        int r = ch_reactor_poll(&rt->reactor, wait_ms == 0 ? 1 : wait_ms, &payload);
        if (r > 0) {
            if (ch_is_fiber(payload)) {
                ChFiber *f = ch_as_fiber(payload);
                if (f->state == CH_FIBER_WAITING || f->state == CH_FIBER_IO_WAITING) {
                    f->state = CH_FIBER_READY;
                    (void)enqueue_ready_fiber(rt, payload);
                }
            } else if (payload == done) {
                return 0;
            }
        }
        while (rt->ready_count > 0) {
            int ran = run_next_ready_fiber(vm);
            if (ran < 0) {
                return -1;
            }
            if (ran == 0) {
                break;
            }
        }
    }
}

int ch_fiber_wait_fd(ChVM *vm, int fd, ChReactorInterest interest) {
    if (!vm || !vm->fiber_runtime) {
        snprintf(vm->error, sizeof(vm->error), "fiber-wait-fd: fiber runtime unavailable");
        return -1;
    }
    if (fd < 0) {
        snprintf(vm->error, sizeof(vm->error), "fiber-wait-fd: invalid fd");
        return -1;
    }
    /* Nested cooperative wait: block this C frame until the fd is ready while
     * still driving sibling fibers. Snapshot-park is a poor fit for port
     * natives that must retry the read/write after wake. */
    ChFiberRuntime *rt = vm->fiber_runtime;
    ChValue done = CH_TRUE;
    if (ch_reactor_register_fd(&rt->reactor, fd, interest, done) != 0) {
        snprintf(vm->error, sizeof(vm->error), "fiber-wait-fd: register failed");
        return -1;
    }
    for (;;) {
        /* Drive siblings first so a peer can unblock this fd. */
        while (rt->ready_count > 0) {
            int ran = run_next_ready_fiber(vm);
            if (ran < 0) {
                (void)ch_reactor_unregister_fd(&rt->reactor, fd);
                return -1;
            }
            if (ran == 0) {
                break;
            }
        }
        ChValue payload = CH_UNDEFINED;
        uint64_t wait_ms = next_reactor_wait_ms(&rt->reactor);
        if (wait_ms == 0) {
            wait_ms = 50;
        }
        int r = ch_reactor_poll(&rt->reactor, wait_ms, &payload);
        if (r > 0) {
            if (payload == done) {
                return 0;
            }
            if (ch_is_fiber(payload)) {
                ChFiber *f = ch_as_fiber(payload);
                if (f->state == CH_FIBER_WAITING || f->state == CH_FIBER_IO_WAITING) {
                    f->state = CH_FIBER_READY;
                    (void)enqueue_ready_fiber(rt, payload);
                }
            }
        }
    }
}

int run_scheduler_step_until(ChVM *vm, uint64_t deadline_ms) {
    if (!vm || !vm->fiber_runtime) {
        struct timespec nap = {0, 1000000L};
        nanosleep(&nap, NULL);
        return 0;
    }

    ChFiberRuntime *rt = vm->fiber_runtime;
    (void)poll_reactor_wake(vm, 0);
    if (rt->ready_count > 0) {
        return run_next_ready_fiber(vm);
    }

    if (timeout_expired_ms(deadline_ms)) {
        return 0;
    }

    uint64_t now = ch_reactor_now_ms();
    uint64_t remain = deadline_ms > now ? deadline_ms - now : 0;
    uint64_t wait_ms = next_reactor_wait_ms(&rt->reactor);
    if (wait_ms == 0 || wait_ms > remain) {
        wait_ms = remain;
    }
    (void)poll_reactor_wake(vm, wait_ms == 0 ? 1 : wait_ms);
    if (rt->ready_count > 0) {
        return run_next_ready_fiber(vm);
    }
    return 0;
}
