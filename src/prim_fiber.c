#include "chaaya/prim.h"

#include "chaaya/fiber.h"
#include "chaaya/rational.h"
#include "chaaya/thread.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static void define_prim(ChVM *vm, const char *name, ChNativeFn fn, int arity, int min_arity) {
    ChValue sym = ch_gc_intern_symbol_cstr(&vm->gc, name);
    int idx = ch_vm_intern_global(vm, ch_as_symbol(sym));
    ChValue nv = ch_gc_make_native(&vm->gc, fn, name, arity, min_arity);
    ch_vm_define_global(vm, idx, nv);
}

static int sleep_seconds_arg(ChValue v, double *out);

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
    int rendezvous = 0;
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
        rendezvous = (n == 0);
    }
    return ch_gc_make_channel(&vm->gc, capacity, rendezvous);
}

static ChValue prim_channel_close(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    if (ch_channel_close(vm, args[0]) != 0) {
        return CH_UNDEFINED;
    }
    return CH_VOID;
}

static ChValue prim_channel_closed_p(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    int closed = 0;
    if (ch_channel_closed(vm, args[0], &closed) != 0) {
        return CH_UNDEFINED;
    }
    return closed ? CH_TRUE : CH_FALSE;
}

static ChValue prim_channel_send(ChVM *vm, ChValue *args, int nargs) {
    if (nargs < 2 || nargs > 4) {
        snprintf(vm->error, sizeof(vm->error),
                 "channel-send: expected 2 to 4 arguments");
        return CH_UNDEFINED;
    }
    if (nargs == 2) {
        if (ch_channel_send(vm, args[0], args[1]) != 0) {
            return CH_UNDEFINED;
        }
        return CH_VOID;
    }

    double timeout = 0.0;
    if (args[2] == CH_FALSE) {
        timeout = 0.0;
    } else if (!sleep_seconds_arg(args[2], &timeout) || !isfinite(timeout) || timeout < 0.0) {
        snprintf(vm->error, sizeof(vm->error),
                 "channel-send: expected non-negative timeout");
        return CH_UNDEFINED;
    }

    int timed_out = 0;
    if (ch_channel_send_timeout(vm, args[0], args[1], timeout, &timed_out) != 0) {
        if (timed_out) {
            if (nargs >= 4) {
                return args[3];
            }
            ChValue msg = ch_gc_make_string_cstr(&vm->gc, "channel-send: timed out");
            ChValue irritants = CH_NIL;
            ch_gc_push(&vm->gc, &msg);
            ch_gc_push(&vm->gc, &irritants);
            ChValue channel_arg = args[0];
            ch_gc_push(&vm->gc, &channel_arg);
            irritants = ch_gc_cons(&vm->gc, channel_arg, CH_NIL);
            ch_gc_pop(&vm->gc);
            ChValue err = ch_gc_make_error_object(&vm->gc, msg, irritants, 0);
            ch_gc_pop_n(&vm->gc, 2);
            return ch_vm_raise(vm, err, 0);
        }
        return CH_UNDEFINED;
    }
    return CH_VOID;
}

static ChValue prim_channel_recv(ChVM *vm, ChValue *args, int nargs) {
    if (nargs < 1 || nargs > 3) {
        snprintf(vm->error, sizeof(vm->error),
                 "channel-receive: expected 1 to 3 arguments");
        return CH_UNDEFINED;
    }
    ChValue value = CH_UNDEFINED;
    if (nargs == 1) {
        if (ch_channel_recv(vm, args[0], &value) != 0) {
            return CH_UNDEFINED;
        }
        return value;
    }

    double timeout = 0.0;
    if (args[1] == CH_FALSE) {
        timeout = 0.0;
    } else if (!sleep_seconds_arg(args[1], &timeout) || !isfinite(timeout) || timeout < 0.0) {
        snprintf(vm->error, sizeof(vm->error),
                 "channel-receive: expected non-negative timeout");
        return CH_UNDEFINED;
    }

    int timed_out = 0;
    if (ch_channel_recv_timeout(vm, args[0], timeout, &value, &timed_out) != 0) {
        if (timed_out) {
            if (nargs >= 3) {
                return args[2];
            }
            ChValue msg = ch_gc_make_string_cstr(&vm->gc, "channel-receive: timed out");
            ChValue irritants = CH_NIL;
            ch_gc_push(&vm->gc, &msg);
            ch_gc_push(&vm->gc, &irritants);
            ChValue channel_arg = args[0];
            ch_gc_push(&vm->gc, &channel_arg);
            irritants = ch_gc_cons(&vm->gc, channel_arg, CH_NIL);
            ch_gc_pop(&vm->gc);
            ChValue err = ch_gc_make_error_object(&vm->gc, msg, irritants, 0);
            ch_gc_pop_n(&vm->gc, 2);
            return ch_vm_raise(vm, err, 0);
        }
        return CH_UNDEFINED;
    }
    return value;
}

static ChValue prim_channel_timeout_exception_p(ChVM *vm, ChValue *args, int nargs) {
    (void)vm;
    (void)nargs;
    if (!ch_is_error_object(args[0])) {
        return CH_FALSE;
    }
    ChErrorObject *err = ch_as_error_object(args[0]);
    if (!ch_is_string(err->message)) {
        return CH_FALSE;
    }
    const char *msg = ch_as_string(err->message)->data;
    if (strncmp(msg, "channel-send: timed out", 23) == 0 ||
        strncmp(msg, "channel-receive: timed out", 26) == 0) {
        return CH_TRUE;
    }
    return CH_FALSE;
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

static ChValue prim_fiber_join(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    ChValue result = CH_UNDEFINED;
    if (ch_fiber_join(vm, args[0], &result) != 0) {
        return CH_UNDEFINED;
    }
    return result;
}

static ChValue prim_make_thread(ChVM *vm, ChValue *args, int nargs) {
    ChValue name = CH_FALSE;
    if (nargs >= 2) {
        name = args[1];
    }
    ChValue out = CH_NIL;
    if (ch_thread_make(vm, args[0], name, &out) != 0) {
        return CH_UNDEFINED;
    }
    return out;
}

static ChValue prim_thread_start(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    if (ch_thread_start(vm, args[0]) != 0) {
        return CH_UNDEFINED;
    }
    return args[0];
}

static ChValue prim_thread_join(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    ChValue result = CH_UNDEFINED;
    if (ch_thread_join(vm, args[0], &result) != 0) {
        return CH_UNDEFINED;
    }
    return result;
}

static int sleep_seconds_arg(ChValue v, double *out) {
    if (ch_is_fixnum(v)) {
        *out = (double)ch_to_fixnum(v);
        return 1;
    }
    if (ch_is_flonum(v)) {
        *out = ch_to_flonum(v);
        return 1;
    }
    if (ch_is_bignum(v) || ch_is_rational_obj(v)) {
        *out = ch_exact_to_f64(v);
        return 1;
    }
    return 0;
}

static ChValue prim_thread_sleep(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    if (args[0] == CH_FALSE) {
        snprintf(vm->error, sizeof(vm->error), "thread-sleep!: expected number");
        return CH_UNDEFINED;
    }
    double seconds = 0.0;
    if (!sleep_seconds_arg(args[0], &seconds)) {
        snprintf(vm->error, sizeof(vm->error), "thread-sleep!: expected number");
        return CH_UNDEFINED;
    }
    if (ch_fiber_sleep(vm, seconds) != 0) {
        return CH_UNDEFINED;
    }
    return CH_VOID;
}

static ChValue prim_thread_yield(ChVM *vm, ChValue *args, int nargs) {
    (void)args;
    (void)nargs;
    if (vm->fiber_runtime && ch_is_fiber(vm->fiber_runtime->current)) {
        if (ch_fiber_yield(vm) != 0) {
            return CH_UNDEFINED;
        }
    }
    return CH_VOID;
}

static ChValue prim_current_thread(ChVM *vm, ChValue *args, int nargs) {
    (void)args;
    (void)nargs;
    return ch_thread_current(vm);
}

static ChValue prim_thread_p(ChVM *vm, ChValue *args, int nargs) {
    (void)vm;
    (void)nargs;
    return ch_is_fiber(args[0]) ? CH_TRUE : CH_FALSE;
}

static ChValue prim_thread_name(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    if (ch_thread_check_owner(vm, args[0], "thread-name") != 0) {
        return CH_UNDEFINED;
    }
    return ch_as_fiber(args[0])->name;
}

static ChValue prim_make_mutex(ChVM *vm, ChValue *args, int nargs) {
    ChValue name = CH_FALSE;
    if (nargs >= 1) {
        name = args[0];
    }
    return ch_gc_make_mutex(&vm->gc, name);
}

static ChValue prim_mutex_p(ChVM *vm, ChValue *args, int nargs) {
    (void)vm;
    (void)nargs;
    return ch_is_mutex(args[0]) ? CH_TRUE : CH_FALSE;
}

static ChValue prim_mutex_lock(ChVM *vm, ChValue *args, int nargs) {
    double timeout = -1.0;
    if (nargs >= 2) {
        if (args[1] == CH_FALSE) {
            timeout = 0.0;
        } else if (!sleep_seconds_arg(args[1], &timeout)) {
            snprintf(vm->error, sizeof(vm->error), "mutex-lock!: expected timeout");
            return CH_UNDEFINED;
        }
    }
    int rc = ch_mutex_lock(vm, args[0], timeout);
    if (rc < 0) {
        return CH_UNDEFINED;
    }
    return rc ? CH_TRUE : CH_FALSE;
}

static ChValue prim_mutex_unlock(ChVM *vm, ChValue *args, int nargs) {
    if (nargs <= 1) {
        if (ch_mutex_unlock(vm, args[0]) != 0) {
            return CH_UNDEFINED;
        }
        return CH_VOID;
    }
    double timeout = -1.0;
    if (nargs >= 3) {
        if (!sleep_seconds_arg(args[2], &timeout)) {
            snprintf(vm->error, sizeof(vm->error), "mutex-unlock!: expected timeout");
            return CH_UNDEFINED;
        }
    }
    if (ch_mutex_unlock_wait(vm, args[0], args[1], timeout) != 0) {
        return CH_UNDEFINED;
    }
    return CH_VOID;
}

static ChValue prim_make_condvar(ChVM *vm, ChValue *args, int nargs) {
    ChValue name = CH_FALSE;
    if (nargs >= 1) {
        name = args[0];
    }
    return ch_gc_make_condvar(&vm->gc, name);
}

static ChValue prim_condvar_p(ChVM *vm, ChValue *args, int nargs) {
    (void)vm;
    (void)nargs;
    return ch_is_condvar(args[0]) ? CH_TRUE : CH_FALSE;
}

static ChValue prim_condvar_signal(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    if (ch_condvar_signal(vm, args[0]) != 0) {
        return CH_UNDEFINED;
    }
    return CH_VOID;
}

static ChValue prim_condvar_broadcast(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    if (ch_condvar_broadcast(vm, args[0]) != 0) {
        return CH_UNDEFINED;
    }
    return CH_VOID;
}

void ch_register_fiber_primitives(ChVM *vm) {
    define_prim(vm, "spawn-fiber", prim_spawn_fiber, 1, 1);
    define_prim(vm, "spawn", prim_spawn_fiber, 1, 1);
    define_prim(vm, "fiber-yield", prim_fiber_yield, 0, 0);
    define_prim(vm, "yield", prim_fiber_yield, 0, 0);
    define_prim(vm, "fiber?", prim_fiber_p, 1, 1);
    define_prim(vm, "fiber-join", prim_fiber_join, 1, 1);
    define_prim(vm, "make-channel", prim_make_channel, -1, 0);
    define_prim(vm, "channel?", prim_channel_p, 1, 1);
    define_prim(vm, "channel-send!", prim_channel_send, -1, 2);
    define_prim(vm, "channel-send", prim_channel_send, -1, 2);
    define_prim(vm, "channel-recv", prim_channel_recv, -1, 1);
    define_prim(vm, "channel-receive", prim_channel_recv, -1, 1);
    define_prim(vm, "channel-get", prim_channel_recv, -1, 1);
    define_prim(vm, "channel-timeout-exception?", prim_channel_timeout_exception_p, 1, 1);
    define_prim(vm, "channel-close!", prim_channel_close, 1, 1);
    define_prim(vm, "channel-closed?", prim_channel_closed_p, 1, 1);

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
    define_prim(vm, "mutex-lock!", prim_mutex_lock, -1, 1);
    define_prim(vm, "mutex-unlock!", prim_mutex_unlock, -1, 1);
    define_prim(vm, "make-condition-variable", prim_make_condvar, -1, 0);
    define_prim(vm, "condition-variable?", prim_condvar_p, 1, 1);
    define_prim(vm, "condition-variable-signal!", prim_condvar_signal, 1, 1);
    define_prim(vm, "condition-variable-broadcast!", prim_condvar_broadcast, 1, 1);
}
