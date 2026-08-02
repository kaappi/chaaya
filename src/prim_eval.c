#include "chaaya/prim.h"

#include "chaaya/bignum.h"
#include "chaaya/environment.h"
#include "chaaya/eval.h"
#include "chaaya/library.h"

#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

static void define_prim(ChVM *vm, const char *name, ChNativeFn fn, int arity, int min_arity) {
    ChValue sym = ch_gc_intern_symbol_cstr(&vm->gc, name);
    ChSymbol *s = ch_as_symbol(sym);
    int idx = ch_vm_intern_global(vm, s);
    ChValue nv = ch_gc_make_native(&vm->gc, fn, name, arity, min_arity);
    ch_vm_define_global(vm, idx, nv);
}

static int exact_to_i64(ChValue v, int64_t *out) {
    if (ch_is_fixnum(v)) {
        *out = ch_to_fixnum(v);
        return 1;
    }
    if (ch_is_bignum(v)) {
        double d = ch_bignum_to_f64(v);
        if (!isfinite(d) || d < (double)INT64_MIN || d > (double)INT64_MAX || floor(d) != d) {
            return 0;
        }
        *out = (int64_t)d;
        return 1;
    }
    return 0;
}

static int import_library_into_env(ChVM *vm, ChEnvironment *env, const char *a, const char *b) {
    ChValue set = CH_NIL;
    ch_gc_push(&vm->gc, &set);
    set = ch_gc_cons(&vm->gc, ch_gc_intern_symbol_cstr(&vm->gc, b), CH_NIL);
    set = ch_gc_cons(&vm->gc, ch_gc_intern_symbol_cstr(&vm->gc, a), set);
    int rc = ch_import_set_into_env(vm, set, &env->env);
    ch_gc_pop(&vm->gc);
    return rc;
}

static ChValue prim_eval(ChVM *vm, ChValue *args, int nargs) {
    ChValue env = (nargs >= 2) ? args[1] : CH_VOID;
    ChValue result = CH_VOID;
    if (ch_eval_datum(vm, args[0], env, &result) != 0) {
        if (vm->error[0] != '\0') {
            ChValue msg = ch_gc_make_string_cstr(&vm->gc, vm->error);
            ChValue err = ch_gc_make_error_object(&vm->gc, msg, CH_NIL, 0);
            vm->error[0] = '\0';
            return ch_vm_raise(vm, err, 0);
        }
        return CH_UNDEFINED;
    }
    return result;
}

static ChValue prim_environment(ChVM *vm, ChValue *args, int nargs) {
    ChValue envv = ch_gc_make_environment(&vm->gc, CH_ENV_CUSTOM);
    ch_gc_push(&vm->gc, &envv);
    ChEnvironment *env = ch_as_environment(envv);
    if (ch_environment_from_imports(vm, args, nargs, &env->env) != 0) {
        ch_gc_pop(&vm->gc);
        return CH_UNDEFINED;
    }
    ch_gc_pop(&vm->gc);
    return envv;
}

static ChValue prim_interaction_environment(ChVM *vm, ChValue *args, int nargs) {
    (void)args;
    (void)nargs;
    return ch_gc_make_environment(&vm->gc, CH_ENV_INTERACTION);
}

static ChValue prim_null_environment(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    if (!ch_is_fixnum(args[0])) {
        snprintf(vm->error, sizeof(vm->error), "null-environment: expected report version");
        return CH_UNDEFINED;
    }
    if (ch_to_fixnum(args[0]) != 5) {
        snprintf(vm->error, sizeof(vm->error), "null-environment: only report version 5 supported");
        return CH_UNDEFINED;
    }
    return ch_gc_make_environment(&vm->gc, CH_ENV_NULL);
}

static ChValue prim_scheme_report_environment(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    if (!ch_is_fixnum(args[0])) {
        snprintf(vm->error, sizeof(vm->error), "scheme-report-environment: expected report version");
        return CH_UNDEFINED;
    }
    if (ch_to_fixnum(args[0]) != 5) {
        snprintf(vm->error, sizeof(vm->error),
                 "scheme-report-environment: only report version 5 supported");
        return CH_UNDEFINED;
    }
    ChValue envv = ch_gc_make_environment(&vm->gc, CH_ENV_REPORT);
    ch_gc_push(&vm->gc, &envv);
    ChEnvironment *env = ch_as_environment(envv);
    if (import_library_into_env(vm, env, "scheme", "r5rs") != 0) {
        ch_gc_pop(&vm->gc);
        return CH_UNDEFINED;
    }
    ch_gc_pop(&vm->gc);
    return envv;
}

static ChValue prim_load(ChVM *vm, ChValue *args, int nargs) {
    if (nargs > 2) {
        snprintf(vm->error, sizeof(vm->error), "load: expected 1 or 2 arguments");
        return CH_UNDEFINED;
    }
    if (!ch_is_string(args[0])) {
        snprintf(vm->error, sizeof(vm->error), "load: expected string path");
        return CH_UNDEFINED;
    }
    ChValue env = (nargs == 2) ? args[1] : CH_VOID;
    ChValue result = CH_VOID;
    if (ch_eval_file(vm, ch_as_string(args[0])->data, env, &result) != 0) {
        ChValue msg = ch_gc_make_string_cstr(&vm->gc, vm->error);
        ChValue irritants = CH_NIL;
        ch_gc_push(&vm->gc, &msg);
        ch_gc_push(&vm->gc, &irritants);
        ch_gc_push(&vm->gc, &args[0]);
        irritants = ch_gc_cons(&vm->gc, args[0], CH_NIL);
        ch_gc_pop(&vm->gc);
        ChValue err = ch_gc_make_error_object(&vm->gc, msg, irritants, 1);
        ch_gc_pop_n(&vm->gc, 2);
        return ch_vm_raise(vm, err, 0);
    }
    return result;
}

static ChValue prim_make_parameter(ChVM *vm, ChValue *args, int nargs) {
    if (nargs > 2) {
        snprintf(vm->error, sizeof(vm->error), "make-parameter: expected 1 or 2 arguments");
        return CH_UNDEFINED;
    }
    ChValue init = args[0];
    ChValue converter = (nargs == 2) ? args[1] : CH_NIL;
    if (!ch_is_nil(converter) && !ch_is_procedure(converter)) {
        snprintf(vm->error, sizeof(vm->error), "make-parameter: converter must be a procedure");
        return CH_UNDEFINED;
    }
    if (!ch_is_nil(converter)) {
        ChValue converted = CH_VOID;
        ChValue call_args[1] = {init};
        ChVMStatus st = ch_vm_apply(vm, converter, call_args, 1, &converted);
        if (st == CH_VM_CONTINUATION_INVOKED) {
            vm->continuation_invoked = true;
            return CH_UNDEFINED;
        }
        if (st != CH_VM_OK) {
            return CH_UNDEFINED;
        }
        init = converted;
    }
    return ch_gc_make_parameter(&vm->gc, init, converter);
}

static ChValue prim_parameter_convert(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    if (!ch_is_parameter(args[0])) {
        snprintf(vm->error, sizeof(vm->error), "%%parameter-convert: expected parameter");
        return CH_UNDEFINED;
    }
    ChParameter *param = ch_as_parameter(args[0]);
    if (ch_is_nil(param->converter)) {
        return args[1];
    }
    ChValue result = CH_VOID;
    ChValue call_args[1] = {args[1]};
    ChVMStatus st = ch_vm_apply(vm, param->converter, call_args, 1, &result);
    if (st == CH_VM_CONTINUATION_INVOKED) {
        vm->continuation_invoked = true;
        return CH_UNDEFINED;
    }
    if (st != CH_VM_OK) {
        return CH_UNDEFINED;
    }
    return result;
}

static ChValue prim_parameter_push(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    if (!ch_is_parameter(args[0])) {
        snprintf(vm->error, sizeof(vm->error), "%%parameter-push!: expected parameter");
        return CH_UNDEFINED;
    }
    if (ch_vm_parameter_push(vm, args[0], args[1]) != 0) {
        return CH_UNDEFINED;
    }
    return CH_VOID;
}

static ChValue prim_parameter_pop(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    if (!ch_is_parameter(args[0])) {
        snprintf(vm->error, sizeof(vm->error), "%%parameter-pop!: expected parameter");
        return CH_UNDEFINED;
    }
    if (ch_vm_parameter_pop(vm, args[0]) != 0) {
        return CH_UNDEFINED;
    }
    return CH_VOID;
}

static int read_current_time(ChVM *vm, struct timespec *ts) {
    if (timespec_get(ts, TIME_UTC) == 0) {
        snprintf(vm->error, sizeof(vm->error), "time: could not read system clock");
        return 0;
    }
    return 1;
}

static ChValue prim_current_second(ChVM *vm, ChValue *args, int nargs) {
    (void)args;
    (void)nargs;
    struct timespec ts;
    if (!read_current_time(vm, &ts)) {
        return CH_UNDEFINED;
    }
    double seconds = (double)ts.tv_sec + ((double)ts.tv_nsec / 1000000000.0);
    return ch_make_flonum(seconds);
}

static ChValue prim_current_jiffy(ChVM *vm, ChValue *args, int nargs) {
    (void)args;
    (void)nargs;
    struct timespec ts;
    if (!read_current_time(vm, &ts)) {
        return CH_UNDEFINED;
    }
    ChValue secs = ch_make_integer(&vm->gc, (int64_t)ts.tv_sec);
    ChValue nanos = ch_make_fixnum((int64_t)ts.tv_nsec);
    ch_gc_push(&vm->gc, &secs);
    ch_gc_push(&vm->gc, &nanos);
    ChValue ticks = ch_bignum_mul(&vm->gc, secs, ch_make_fixnum(1000000000LL));
    ch_gc_push(&vm->gc, &ticks);
    ticks = ch_bignum_add(&vm->gc, ticks, nanos);
    ch_gc_pop_n(&vm->gc, 3);
    return ticks;
}

static ChValue prim_jiffies_per_second(ChVM *vm, ChValue *args, int nargs) {
    (void)vm;
    (void)args;
    (void)nargs;
    return ch_make_fixnum(1000000000LL);
}

static ChValue prim_time_p(ChVM *vm, ChValue *args, int nargs) {
    (void)vm;
    (void)nargs;
    return ch_is_time(args[0]) ? CH_TRUE : CH_FALSE;
}

static ChValue prim_make_time(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    if (!ch_is_symbol(args[0])) {
        snprintf(vm->error, sizeof(vm->error), "make-time: expected time type symbol");
        return CH_UNDEFINED;
    }
    if (!ch_is_exact_integer(args[1]) || !ch_is_exact_integer(args[2])) {
        snprintf(vm->error, sizeof(vm->error), "make-time: expected exact integers");
        return CH_UNDEFINED;
    }

    int64_t a = 0;
    int64_t b = 0;
    if (!exact_to_i64(args[1], &a) || !exact_to_i64(args[2], &b)) {
        snprintf(vm->error, sizeof(vm->error), "make-time: integer argument out of range");
        return CH_UNDEFINED;
    }

    int64_t nanoseconds = a;
    int64_t seconds = b;
    if ((nanoseconds < 0 || nanoseconds >= 1000000000LL) &&
        (b >= 0 && b < 1000000000LL)) {
        seconds = a;
        nanoseconds = b;
    }
    if (nanoseconds < 0 || nanoseconds >= 1000000000LL) {
        snprintf(vm->error, sizeof(vm->error), "make-time: nanosecond out of range");
        return CH_UNDEFINED;
    }
    return ch_gc_make_time(&vm->gc, seconds, (int32_t)nanoseconds, args[0]);
}

static ChValue prim_time_type(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    if (!ch_is_time(args[0])) {
        snprintf(vm->error, sizeof(vm->error), "time-type: expected time object");
        return CH_UNDEFINED;
    }
    return ch_as_time(args[0])->type_sym;
}

static ChValue prim_time_second(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    if (!ch_is_time(args[0])) {
        snprintf(vm->error, sizeof(vm->error), "time-second: expected time object");
        return CH_UNDEFINED;
    }
    return ch_make_integer(&vm->gc, ch_as_time(args[0])->seconds);
}

static ChValue prim_time_nanosecond(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    if (!ch_is_time(args[0])) {
        snprintf(vm->error, sizeof(vm->error), "time-nanosecond: expected time object");
        return CH_UNDEFINED;
    }
    return ch_make_fixnum(ch_as_time(args[0])->nanoseconds);
}

void ch_register_eval_primitives(ChVM *vm) {
    define_prim(vm, "eval", prim_eval, -1, 1);
    define_prim(vm, "environment", prim_environment, -1, 0);
    define_prim(vm, "interaction-environment", prim_interaction_environment, 0, 0);
    define_prim(vm, "null-environment", prim_null_environment, 1, 1);
    define_prim(vm, "scheme-report-environment", prim_scheme_report_environment, 1, 1);
    define_prim(vm, "load", prim_load, -1, 1);
    define_prim(vm, "make-parameter", prim_make_parameter, -1, 1);
    define_prim(vm, "%parameter-convert", prim_parameter_convert, 2, 2);
    define_prim(vm, "%parameter-push!", prim_parameter_push, 2, 2);
    define_prim(vm, "%parameter-pop!", prim_parameter_pop, 1, 1);
    define_prim(vm, "current-second", prim_current_second, 0, 0);
    define_prim(vm, "current-jiffy", prim_current_jiffy, 0, 0);
    define_prim(vm, "jiffies-per-second", prim_jiffies_per_second, 0, 0);
    define_prim(vm, "time?", prim_time_p, 1, 1);
    define_prim(vm, "make-time", prim_make_time, 3, 3);
    define_prim(vm, "time-type", prim_time_type, 1, 1);
    define_prim(vm, "time-second", prim_time_second, 1, 1);
    define_prim(vm, "time-nanosecond", prim_time_nanosecond, 1, 1);
}
