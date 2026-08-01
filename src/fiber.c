#include "chaaya/fiber.h"

#include "chaaya/vm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int enqueue_ready_fiber(ChFiberRuntime *runtime, ChValue fiber) {
    if (!runtime || runtime->ready_count >= CH_FIBER_READY_MAX) {
        return -1;
    }
    size_t tail = (runtime->ready_head + runtime->ready_count) % CH_FIBER_READY_MAX;
    runtime->ready[tail] = fiber;
    runtime->ready_count++;
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
    return runtime->ready_count + 1 + runtime->reactor.timer_count;
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
    memset(fiber->reserved, 0, sizeof(fiber->reserved));
    fiber->thunk = thunk;
    fiber->result = CH_UNDEFINED;
    fiber->error = CH_NIL;
    return ch_make_pointer(&fiber->header);
}

ChValue ch_gc_make_channel(ChGC *gc, size_t capacity) {
    ChChannel *channel = (ChChannel *)ch_gc_alloc(gc, sizeof(ChChannel), CH_TAG_CHANNEL);
    channel->capacity = capacity;
    channel->storage_cap = capacity == 0 ? 8 : capacity;
    channel->count = 0;
    channel->head = 0;
    channel->tail = 0;
    channel->items = (ChValue *)calloc(channel->storage_cap, sizeof(ChValue));
    if (!channel->items) {
        abort();
    }
    for (size_t i = 0; i < channel->storage_cap; i++) {
        channel->items[i] = CH_UNDEFINED;
    }
    return ch_make_pointer(&channel->header);
}

static int run_next_ready_fiber(ChVM *vm) {
    ChFiberRuntime *runtime = vm->fiber_runtime;
    ChValue fiber_v = CH_NIL;
    int has_fiber = dequeue_ready_fiber(runtime, &fiber_v);
    if (has_fiber <= 0) {
        return 0;
    }
    if (!ch_is_fiber(fiber_v)) {
        return 0;
    }

    ch_gc_push(&vm->gc, &fiber_v);
    ChFiber *fiber = ch_as_fiber(fiber_v);
    if (fiber->state != CH_FIBER_READY) {
        ch_gc_pop(&vm->gc);
        return 0;
    }

    ChValue previous_current = runtime->current;
    runtime->current = fiber_v;
    fiber->state = CH_FIBER_RUNNING;

    ChValue result = CH_VOID;
    ChVMStatus st = ch_vm_apply(vm, fiber->thunk, NULL, 0, &result);
    if (st == CH_VM_OK) {
        fiber->state = CH_FIBER_DONE;
        fiber->result = ch_coerce_single(result);
        fiber->error = CH_NIL;
        ch_gc_write_barrier(&vm->gc, &fiber->header, fiber->result);
    } else if (st == CH_VM_CONTINUATION_INVOKED) {
        fiber->state = CH_FIBER_FAILED;
        fiber->result = CH_UNDEFINED;
        fiber->error =
            ch_gc_make_string_cstr(&vm->gc, "fiber: continuations across scheduler are unsupported");
        ch_gc_write_barrier(&vm->gc, &fiber->header, fiber->error);
        snprintf(vm->error, sizeof(vm->error),
                 "fiber: continuations across scheduler are unsupported");
        runtime->current = previous_current;
        ch_gc_pop(&vm->gc);
        return -1;
    } else {
        fiber->state = CH_FIBER_FAILED;
        fiber->result = CH_UNDEFINED;
        const char *msg = vm->error[0] ? vm->error : "fiber failed";
        fiber->error = ch_gc_make_string_cstr(&vm->gc, msg);
        ch_gc_write_barrier(&vm->gc, &fiber->header, fiber->error);
        runtime->current = previous_current;
        ch_gc_pop(&vm->gc);
        return -1;
    }

    runtime->current = previous_current;
    ch_gc_pop(&vm->gc);
    return 1;
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
    (void)ch_reactor_poll(&vm->fiber_runtime->reactor, 0, NULL);
    int ran = run_next_ready_fiber(vm);
    if (ran < 0) {
        return -1;
    }
    return 0;
}

static int channel_grow(ChChannel *channel) {
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

static void channel_push(ChChannel *channel, ChValue value) {
    channel->items[channel->tail] = value;
    channel->tail = (channel->tail + 1) % channel->storage_cap;
    channel->count++;
}

static ChValue channel_pop(ChChannel *channel) {
    ChValue value = channel->items[channel->head];
    channel->items[channel->head] = CH_UNDEFINED;
    channel->head = (channel->head + 1) % channel->storage_cap;
    channel->count--;
    return value;
}

int ch_channel_send(ChVM *vm, ChValue channel_v, ChValue value) {
    if (!ch_is_channel(channel_v)) {
        snprintf(vm->error, sizeof(vm->error), "channel-send!: expected channel");
        return -1;
    }
    ChChannel *channel = ch_as_channel(channel_v);
    size_t spins = 0;
    while (channel->capacity > 0 && channel->count >= channel->capacity) {
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
    if (channel->capacity == 0 && channel->count >= channel->storage_cap) {
        if (channel_grow(channel) != 0) {
            snprintf(vm->error, sizeof(vm->error), "channel-send!: out of memory");
            return -1;
        }
    }
    channel_push(channel, value);
    ch_gc_write_barrier(&vm->gc, &channel->header, value);
    return 0;
}

int ch_channel_recv(ChVM *vm, ChValue channel_v, ChValue *out_value) {
    if (!ch_is_channel(channel_v)) {
        snprintf(vm->error, sizeof(vm->error), "channel-recv: expected channel");
        return -1;
    }
    ChChannel *channel = ch_as_channel(channel_v);
    size_t spins = 0;
    while (channel->count == 0) {
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
    return 0;
}
