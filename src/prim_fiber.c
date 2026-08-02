#include "chaaya/prim.h"

#include "chaaya/fiber.h"
#include "chaaya/rational.h"
#include "chaaya/sandbox.h"
#include "chaaya/thread.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#if !defined(_WIN32)
#include <unistd.h>
#endif

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

/* Returns the error-object message iff args[0] is an error-object whose
 * message starts with `prefix`, else NULL. */
static const char *error_object_message_with_prefix(ChValue v, const char *prefix) {
    if (!ch_is_error_object(v)) {
        return NULL;
    }
    ChErrorObject *err = ch_as_error_object(v);
    if (!ch_is_string(err->message)) {
        return NULL;
    }
    const char *msg = ch_as_string(err->message)->data;
    size_t plen = strlen(prefix);
    return strncmp(msg, prefix, plen) == 0 ? msg : NULL;
}

static ChValue prim_join_timeout_exception_p(ChVM *vm, ChValue *args, int nargs) {
    (void)vm;
    (void)nargs;
    return error_object_message_with_prefix(args[0], "thread-join!: timed out") ? CH_TRUE : CH_FALSE;
}

static ChValue prim_terminated_thread_exception_p(ChVM *vm, ChValue *args, int nargs) {
    (void)vm;
    (void)nargs;
    return error_object_message_with_prefix(args[0], "thread-join!: terminated-thread-exception")
              ? CH_TRUE
              : CH_FALSE;
}

static ChValue prim_abandoned_mutex_exception_p(ChVM *vm, ChValue *args, int nargs) {
    (void)vm;
    (void)nargs;
    return error_object_message_with_prefix(args[0], "mutex-lock!: abandoned mutex") ? CH_TRUE
                                                                                     : CH_FALSE;
}

#define CH_UNCAUGHT_EXCEPTION_PREFIX "uncaught exception: "

static ChValue prim_uncaught_exception_p(ChVM *vm, ChValue *args, int nargs) {
    (void)vm;
    (void)nargs;
    return error_object_message_with_prefix(args[0], CH_UNCAUGHT_EXCEPTION_PREFIX) ? CH_TRUE
                                                                                   : CH_FALSE;
}

/* The original raised value cannot in general cross the OS-thread boundary
 * that separates the (uncaught) raise from thread-join! observing it — see
 * ch_vm_raise's handler_count==0 branch, which keeps only a printed string.
 * uncaught-exception-reason therefore returns that printed representation
 * as a string rather than the original condition object. */
static ChValue prim_uncaught_exception_reason(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    const char *msg = error_object_message_with_prefix(args[0], CH_UNCAUGHT_EXCEPTION_PREFIX);
    if (!msg) {
        snprintf(vm->error, sizeof(vm->error),
                 "uncaught-exception-reason: expected an uncaught-exception");
        return CH_UNDEFINED;
    }
    return ch_gc_make_string_cstr(&vm->gc, msg + (sizeof(CH_UNCAUGHT_EXCEPTION_PREFIX) - 1));
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

/* Re-raise a failed fiber's stashed condition in the joiner's handler
 * context (#564). Strings become error-objects so guard/error-object?
 * see a proper condition. */
static ChValue raise_fiber_join_error(ChVM *vm, ChFiber *fiber) {
    ChValue err = fiber->error;
    if (ch_is_error_object(err)) {
        vm->error[0] = '\0';
        return ch_vm_raise(vm, err, 0);
    }
    if (ch_is_string(err)) {
        ch_gc_push(&vm->gc, &err);
        ChValue obj = ch_gc_make_error_object(&vm->gc, err, CH_NIL, 0);
        ch_gc_pop(&vm->gc);
        vm->error[0] = '\0';
        return ch_vm_raise(vm, obj, 0);
    }
    if (err != CH_NIL && err != CH_UNDEFINED) {
        vm->error[0] = '\0';
        return ch_vm_raise(vm, err, 0);
    }
    if (vm->error[0] == '\0') {
        snprintf(vm->error, sizeof(vm->error), "fiber-join: fiber failed");
    }
    return CH_UNDEFINED;
}

static ChValue prim_fiber_join(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    ChValue fiber_v = args[0];
    ChValue result = CH_UNDEFINED;
    if (ch_fiber_join(vm, fiber_v, &result) != 0) {
        if (ch_is_fiber(fiber_v) && ch_as_fiber(fiber_v)->state == CH_FIBER_FAILED) {
            return raise_fiber_join_error(vm, ch_as_fiber(fiber_v));
        }
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
    if (nargs <= 1) {
        ChValue thread = args[0];
        ChValue result = CH_UNDEFINED;
        if (ch_thread_join(vm, thread, &result) != 0) {
            if (ch_is_fiber(thread) && ch_as_fiber(thread)->state == CH_FIBER_FAILED) {
                return raise_fiber_join_error(vm, ch_as_fiber(thread));
            }
            return CH_UNDEFINED;
        }
        return result;
    }

    double timeout = 0.0;
    if (args[1] == CH_FALSE) {
        timeout = 0.0;
    } else if (!sleep_seconds_arg(args[1], &timeout) || !isfinite(timeout) || timeout < 0.0) {
        snprintf(vm->error, sizeof(vm->error), "thread-join!: expected non-negative timeout");
        return CH_UNDEFINED;
    }

    int timed_out = 0;
    ChValue result = CH_UNDEFINED;
    if (ch_thread_join_timeout(vm, args[0], timeout, &timed_out, &result) != 0) {
        if (timed_out) {
            if (nargs >= 3) {
                return args[2];
            }
            ChValue msg = ch_gc_make_string_cstr(&vm->gc, "thread-join!: timed out");
            ChValue irritants = CH_NIL;
            ch_gc_push(&vm->gc, &msg);
            ch_gc_push(&vm->gc, &irritants);
            ChValue thread_arg = args[0];
            ch_gc_push(&vm->gc, &thread_arg);
            irritants = ch_gc_cons(&vm->gc, thread_arg, CH_NIL);
            ch_gc_pop(&vm->gc);
            ChValue err = ch_gc_make_error_object(&vm->gc, msg, irritants, 0);
            ch_gc_pop_n(&vm->gc, 2);
            return ch_vm_raise(vm, err, 0);
        }
        return CH_UNDEFINED;
    }
    return result;
}

/* Accepts a plain number of seconds (relative), or an SRFI-18 time object
 * (an absolute deadline, per SRFI-18 semantics for timeout arguments) which
 * is converted to a relative "seconds from now" (clamped to >= 0). */
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
    if (ch_is_time(v)) {
        ChTime *t = ch_as_time(v);
        double deadline = (double)t->seconds + ((double)t->nanoseconds / 1e9);
        struct timespec now_ts;
        if (timespec_get(&now_ts, TIME_UTC) == 0) {
            return 0;
        }
        double now = (double)now_ts.tv_sec + ((double)now_ts.tv_nsec / 1e9);
        double rel = deadline - now;
        *out = rel > 0.0 ? rel : 0.0;
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

static ChValue prim_thread_specific(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    if (ch_thread_check_owner(vm, args[0], "thread-specific") != 0) {
        return CH_UNDEFINED;
    }
    return ch_as_fiber(args[0])->specific;
}

static ChValue prim_thread_specific_set(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    if (ch_thread_check_owner(vm, args[0], "thread-specific-set!") != 0) {
        return CH_UNDEFINED;
    }
    ChFiber *f = ch_as_fiber(args[0]);
    f->specific = args[1];
    ch_gc_write_barrier(&vm->gc, &f->header, args[1]);
    return CH_VOID;
}

static ChValue prim_thread_terminate(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    if (ch_thread_terminate(vm, args[0]) != 0) {
        return CH_UNDEFINED;
    }
    return CH_VOID;
}

#if !defined(__wasi__)
static ChValue prim_processor_count(ChVM *vm, ChValue *args, int nargs) {
    (void)vm;
    (void)args;
    (void)nargs;
    if (ch_sandbox_enabled()) {
        return ch_make_fixnum(1);
    }
#if defined(_SC_NPROCESSORS_ONLN)
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n < 1) {
        n = 1;
    }
    return ch_make_fixnum((int64_t)n);
#else
    return ch_make_fixnum(1);
#endif
}
#else
static ChValue prim_processor_count(ChVM *vm, ChValue *args, int nargs) {
    (void)vm;
    (void)args;
    (void)nargs;
    return ch_make_fixnum(1);
}
#endif

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
    /* SRFI 18: (mutex-lock! mutex [timeout [thread]]). Unlike the #f-means-
     * "poll, don't block" convention used by channel-send/receive and
     * thread-join!, mutex-lock!'s timeout defaults to (and #f explicitly
     * means) block indefinitely — leave timeout at -1.0 for both. */
    double timeout = -1.0;
    if (nargs >= 2 && args[1] != CH_FALSE) {
        if (!sleep_seconds_arg(args[1], &timeout)) {
            snprintf(vm->error, sizeof(vm->error), "mutex-lock!: expected timeout");
            return CH_UNDEFINED;
        }
    }
    ChValue owner_override = nargs >= 3 ? args[2] : CH_UNDEFINED;
    int rc = ch_mutex_lock(vm, args[0], timeout, owner_override);
    if (rc == -2) {
        ChValue msg = ch_gc_make_string_cstr(&vm->gc, vm->error[0] ? vm->error
                                                                    : "mutex-lock!: abandoned mutex");
        vm->error[0] = '\0';
        ch_gc_push(&vm->gc, &msg);
        ChValue err = ch_gc_make_error_object(&vm->gc, msg, CH_NIL, 0);
        ch_gc_pop(&vm->gc);
        return ch_vm_raise(vm, err, 0);
    }
    if (rc < 0) {
        return CH_UNDEFINED;
    }
    return rc ? CH_TRUE : CH_FALSE;
}

static ChValue prim_mutex_name(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    if (!ch_is_mutex(args[0])) {
        snprintf(vm->error, sizeof(vm->error), "mutex-name: expected mutex");
        return CH_UNDEFINED;
    }
    return ch_as_mutex(args[0])->name;
}

static ChValue prim_mutex_specific(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    if (!ch_is_mutex(args[0])) {
        snprintf(vm->error, sizeof(vm->error), "mutex-specific: expected mutex");
        return CH_UNDEFINED;
    }
    return ch_as_mutex(args[0])->specific;
}

static ChValue prim_mutex_specific_set(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    if (!ch_is_mutex(args[0])) {
        snprintf(vm->error, sizeof(vm->error), "mutex-specific-set!: expected mutex");
        return CH_UNDEFINED;
    }
    ChMutex *m = ch_as_mutex(args[0]);
    m->specific = args[1];
    ch_gc_write_barrier(&vm->gc, &m->header, args[1]);
    return CH_VOID;
}

static ChValue prim_mutex_state(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    if (!ch_is_mutex(args[0])) {
        snprintf(vm->error, sizeof(vm->error), "mutex-state: expected mutex");
        return CH_UNDEFINED;
    }
    ChMutex *m = ch_as_mutex(args[0]);
    if (m->header.owner != vm->gc.id) {
        snprintf(vm->error, sizeof(vm->error), "mutex-state: mutex belongs to another OS thread");
        return CH_UNDEFINED;
    }
    if (m->locked && ch_is_fiber(m->owner)) {
        ChFiber *owner_f = ch_as_fiber(m->owner);
        if (owner_f->state == CH_FIBER_DONE || owner_f->state == CH_FIBER_FAILED) {
            return ch_gc_intern_symbol_cstr(&vm->gc, "abandoned");
        }
        return m->owner;
    }
    if (m->abandoned) {
        return ch_gc_intern_symbol_cstr(&vm->gc, "abandoned");
    }
    return ch_gc_intern_symbol_cstr(&vm->gc, "not-owned");
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

static ChValue prim_condvar_name(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    if (!ch_is_condvar(args[0])) {
        snprintf(vm->error, sizeof(vm->error), "condition-variable-name: expected condition variable");
        return CH_UNDEFINED;
    }
    return ch_as_condvar(args[0])->name;
}

static ChValue prim_condvar_specific(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    if (!ch_is_condvar(args[0])) {
        snprintf(vm->error, sizeof(vm->error),
                 "condition-variable-specific: expected condition variable");
        return CH_UNDEFINED;
    }
    return ch_as_condvar(args[0])->specific;
}

static ChValue prim_condvar_specific_set(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    if (!ch_is_condvar(args[0])) {
        snprintf(vm->error, sizeof(vm->error),
                 "condition-variable-specific-set!: expected condition variable");
        return CH_UNDEFINED;
    }
    ChCondvar *c = ch_as_condvar(args[0]);
    c->specific = args[1];
    ch_gc_write_barrier(&vm->gc, &c->header, args[1]);
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
    define_prim(vm, "thread-specific", prim_thread_specific, 1, 1);
    define_prim(vm, "thread-specific-set!", prim_thread_specific_set, 2, 2);
    define_prim(vm, "thread-terminate!", prim_thread_terminate, 1, 1);
    define_prim(vm, "join-timeout-exception?", prim_join_timeout_exception_p, 1, 1);
    define_prim(vm, "terminated-thread-exception?", prim_terminated_thread_exception_p, 1, 1);
    define_prim(vm, "uncaught-exception?", prim_uncaught_exception_p, 1, 1);
    define_prim(vm, "uncaught-exception-reason", prim_uncaught_exception_reason, 1, 1);
    define_prim(vm, "abandoned-mutex-exception?", prim_abandoned_mutex_exception_p, 1, 1);
    define_prim(vm, "processor-count", prim_processor_count, 0, 0);
    define_prim(vm, "make-mutex", prim_make_mutex, -1, 0);
    define_prim(vm, "mutex?", prim_mutex_p, 1, 1);
    define_prim(vm, "mutex-lock!", prim_mutex_lock, -1, 1);
    define_prim(vm, "mutex-unlock!", prim_mutex_unlock, -1, 1);
    define_prim(vm, "mutex-name", prim_mutex_name, 1, 1);
    define_prim(vm, "mutex-specific", prim_mutex_specific, 1, 1);
    define_prim(vm, "mutex-specific-set!", prim_mutex_specific_set, 2, 2);
    define_prim(vm, "mutex-state", prim_mutex_state, 1, 1);
    define_prim(vm, "make-condition-variable", prim_make_condvar, -1, 0);
    define_prim(vm, "condition-variable?", prim_condvar_p, 1, 1);
    define_prim(vm, "condition-variable-signal!", prim_condvar_signal, 1, 1);
    define_prim(vm, "condition-variable-broadcast!", prim_condvar_broadcast, 1, 1);
    define_prim(vm, "condition-variable-name", prim_condvar_name, 1, 1);
    define_prim(vm, "condition-variable-specific", prim_condvar_specific, 1, 1);
    define_prim(vm, "condition-variable-specific-set!", prim_condvar_specific_set, 2, 2);
}
