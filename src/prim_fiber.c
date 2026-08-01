#include "chaaya/prim.h"

#include "chaaya/fiber.h"

#include <stdio.h>

static void define_prim(ChVM *vm, const char *name, ChNativeFn fn, int arity, int min_arity) {
    ChValue sym = ch_gc_intern_symbol_cstr(&vm->gc, name);
    int idx = ch_vm_intern_global(vm, ch_as_symbol(sym));
    ChValue nv = ch_gc_make_native(&vm->gc, fn, name, arity, min_arity);
    ch_vm_define_global(vm, idx, nv);
}

static ChValue prim_spawn_fiber(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    ChValue fiber = CH_NIL;
    if (ch_fiber_spawn(vm, args[0], &fiber) != 0) {
        return CH_UNDEFINED;
    }
    return fiber;
}

static ChValue prim_fiber_yield(ChVM *vm, ChValue *args, int nargs) {
    (void)args;
    (void)nargs;
    if (ch_fiber_yield(vm) != 0) {
        return CH_UNDEFINED;
    }
    return CH_VOID;
}

static ChValue prim_make_channel(ChVM *vm, ChValue *args, int nargs) {
    if (nargs > 1) {
        snprintf(vm->error, sizeof(vm->error), "make-channel: expected 0 or 1 arguments");
        return CH_UNDEFINED;
    }
    size_t capacity = 0;
    if (nargs == 1) {
        if (!ch_is_fixnum(args[0])) {
            snprintf(vm->error, sizeof(vm->error), "make-channel: expected non-negative integer");
            return CH_UNDEFINED;
        }
        int64_t n = ch_to_fixnum(args[0]);
        if (n < 0) {
            snprintf(vm->error, sizeof(vm->error), "make-channel: expected non-negative integer");
            return CH_UNDEFINED;
        }
        capacity = (size_t)n;
    }
    return ch_gc_make_channel(&vm->gc, capacity);
}

static ChValue prim_channel_send(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    if (ch_channel_send(vm, args[0], args[1]) != 0) {
        return CH_UNDEFINED;
    }
    return CH_VOID;
}

static ChValue prim_channel_recv(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    ChValue value = CH_UNDEFINED;
    if (ch_channel_recv(vm, args[0], &value) != 0) {
        return CH_UNDEFINED;
    }
    return value;
}

static ChValue prim_fiber_p(ChVM *vm, ChValue *args, int nargs) {
    (void)vm;
    (void)nargs;
    return ch_is_fiber(args[0]) ? CH_TRUE : CH_FALSE;
}

static ChValue prim_channel_p(ChVM *vm, ChValue *args, int nargs) {
    (void)vm;
    (void)nargs;
    return ch_is_channel(args[0]) ? CH_TRUE : CH_FALSE;
}

static ChValue thread_nyi(ChVM *vm, const char *who) {
    snprintf(vm->error, sizeof(vm->error), "%s: SRFI-18 threads are NYI in this MVP", who);
    return CH_UNDEFINED;
}

static ChValue prim_make_thread(ChVM *vm, ChValue *args, int nargs) {
    (void)args;
    (void)nargs;
    return thread_nyi(vm, "make-thread");
}

static ChValue prim_thread_start(ChVM *vm, ChValue *args, int nargs) {
    (void)args;
    (void)nargs;
    return thread_nyi(vm, "thread-start!");
}

static ChValue prim_thread_join(ChVM *vm, ChValue *args, int nargs) {
    (void)args;
    (void)nargs;
    return thread_nyi(vm, "thread-join!");
}

static ChValue prim_thread_sleep(ChVM *vm, ChValue *args, int nargs) {
    (void)args;
    (void)nargs;
    return thread_nyi(vm, "thread-sleep!");
}

static ChValue prim_thread_yield(ChVM *vm, ChValue *args, int nargs) {
    (void)args;
    (void)nargs;
    return thread_nyi(vm, "thread-yield!");
}

static ChValue prim_current_thread(ChVM *vm, ChValue *args, int nargs) {
    (void)args;
    (void)nargs;
    return thread_nyi(vm, "current-thread");
}

static ChValue prim_thread_p(ChVM *vm, ChValue *args, int nargs) {
    (void)vm;
    (void)args;
    (void)nargs;
    return CH_FALSE;
}

static ChValue prim_thread_name(ChVM *vm, ChValue *args, int nargs) {
    (void)args;
    (void)nargs;
    return thread_nyi(vm, "thread-name");
}

static ChValue prim_make_mutex(ChVM *vm, ChValue *args, int nargs) {
    (void)args;
    (void)nargs;
    return thread_nyi(vm, "make-mutex");
}

static ChValue prim_mutex_p(ChVM *vm, ChValue *args, int nargs) {
    (void)vm;
    (void)args;
    (void)nargs;
    return CH_FALSE;
}

void ch_register_fiber_primitives(ChVM *vm) {
    define_prim(vm, "spawn-fiber", prim_spawn_fiber, 1, 1);
    define_prim(vm, "fiber-yield", prim_fiber_yield, 0, 0);
    define_prim(vm, "fiber?", prim_fiber_p, 1, 1);
    define_prim(vm, "make-channel", prim_make_channel, -1, 0);
    define_prim(vm, "channel?", prim_channel_p, 1, 1);
    define_prim(vm, "channel-send!", prim_channel_send, 2, 2);
    define_prim(vm, "channel-recv", prim_channel_recv, 1, 1);

    /* SRFI-18 subset stubs with explicit NYI errors. */
    define_prim(vm, "make-thread", prim_make_thread, -1, 1);
    define_prim(vm, "thread-start!", prim_thread_start, -1, 1);
    define_prim(vm, "thread-join!", prim_thread_join, -1, 1);
    define_prim(vm, "thread-sleep!", prim_thread_sleep, 1, 1);
    define_prim(vm, "thread-yield!", prim_thread_yield, 0, 0);
    define_prim(vm, "current-thread", prim_current_thread, 0, 0);
    define_prim(vm, "thread?", prim_thread_p, 1, 1);
    define_prim(vm, "thread-name", prim_thread_name, 1, 1);
    define_prim(vm, "make-mutex", prim_make_mutex, -1, 0);
    define_prim(vm, "mutex?", prim_mutex_p, 1, 1);
}
