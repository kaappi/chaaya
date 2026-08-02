#include "chaaya/prim.h"

#include "chaaya/bignum.h"
#include "chaaya/complex.h"
#include "chaaya/printer.h"
#include "chaaya/rational.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void define_prim(ChVM *vm, const char *name, ChNativeFn fn, int arity, int min_arity) {
    ChValue sym = ch_gc_intern_symbol_cstr(&vm->gc, name);
    ChSymbol *s = ch_as_symbol(sym);
    int idx = ch_vm_intern_global(vm, s);
    ChValue nv = ch_gc_make_native(&vm->gc, fn, name, arity, min_arity);
    ch_vm_define_global(vm, idx, nv);
}

static ChValue prim_cons(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    return ch_gc_cons(&vm->gc, args[0], args[1]);
}

static ChValue prim_car(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    if (!ch_is_pair(args[0])) {
        snprintf(vm->error, sizeof(vm->error), "car: not a pair");
        return CH_UNDEFINED;
    }
    return ch_car(args[0]);
}

static ChValue prim_cdr(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    if (!ch_is_pair(args[0])) {
        snprintf(vm->error, sizeof(vm->error), "cdr: not a pair");
        return CH_UNDEFINED;
    }
    return ch_cdr(args[0]);
}

static ChValue prim_set_car(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    if (!ch_is_pair(args[0])) {
        snprintf(vm->error, sizeof(vm->error), "set-car!: not a pair");
        return CH_UNDEFINED;
    }
    if (ch_object_is_immutable(&ch_as_pair(args[0])->header)) {
        snprintf(vm->error, sizeof(vm->error), "set-car!: immutable pair");
        return CH_UNDEFINED;
    }
    ChPair *pair = ch_as_pair(args[0]);
    pair->car = args[1];
    ch_gc_write_barrier(&vm->gc, &pair->header, args[1]);
    return CH_VOID;
}

static ChValue prim_set_cdr(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    if (!ch_is_pair(args[0])) {
        snprintf(vm->error, sizeof(vm->error), "set-cdr!: not a pair");
        return CH_UNDEFINED;
    }
    if (ch_object_is_immutable(&ch_as_pair(args[0])->header)) {
        snprintf(vm->error, sizeof(vm->error), "set-cdr!: immutable pair");
        return CH_UNDEFINED;
    }
    ChPair *pair = ch_as_pair(args[0]);
    pair->cdr = args[1];
    ch_gc_write_barrier(&vm->gc, &pair->header, args[1]);
    return CH_VOID;
}

static ChValue prim_pair_p(ChVM *vm, ChValue *args, int nargs) {
    (void)vm;
    (void)nargs;
    return ch_is_pair(args[0]) ? CH_TRUE : CH_FALSE;
}

static ChValue prim_null_p(ChVM *vm, ChValue *args, int nargs) {
    (void)vm;
    (void)nargs;
    return ch_is_nil(args[0]) ? CH_TRUE : CH_FALSE;
}

static ChValue prim_symbol_p(ChVM *vm, ChValue *args, int nargs) {
    (void)vm;
    (void)nargs;
    return ch_is_symbol(args[0]) ? CH_TRUE : CH_FALSE;
}

static ChValue prim_string_p(ChVM *vm, ChValue *args, int nargs) {
    (void)vm;
    (void)nargs;
    return ch_is_string(args[0]) ? CH_TRUE : CH_FALSE;
}

static ChValue prim_number_p(ChVM *vm, ChValue *args, int nargs) {
    (void)vm;
    (void)nargs;
    return ch_is_number(args[0]) ? CH_TRUE : CH_FALSE;
}

static ChValue prim_exact_p(ChVM *vm, ChValue *args, int nargs) {
    (void)vm;
    (void)nargs;
    return ch_is_exact(args[0]) ? CH_TRUE : CH_FALSE;
}

static ChValue prim_exact_integer_p(ChVM *vm, ChValue *args, int nargs) {
    (void)vm;
    (void)nargs;
    return ch_is_exact_integer(args[0]) ? CH_TRUE : CH_FALSE;
}

static ChValue prim_inexact_p(ChVM *vm, ChValue *args, int nargs) {
    (void)vm;
    (void)nargs;
    return (ch_is_flonum(args[0]) || ch_is_complex_obj(args[0])) ? CH_TRUE : CH_FALSE;
}

static ChValue prim_complex_p(ChVM *vm, ChValue *args, int nargs) {
    (void)vm;
    (void)nargs;
    /* R7RS: every number is complex */
    return ch_is_number(args[0]) ? CH_TRUE : CH_FALSE;
}

static ChValue prim_real_p(ChVM *vm, ChValue *args, int nargs) {
    (void)vm;
    (void)nargs;
    return (ch_is_number(args[0]) && !ch_is_complex_obj(args[0])) ? CH_TRUE : CH_FALSE;
}

static ChValue prim_rational_p(ChVM *vm, ChValue *args, int nargs) {
    (void)vm;
    (void)nargs;
    if (ch_is_complex_obj(args[0])) {
        return CH_FALSE;
    }
    if (ch_is_flonum(args[0])) {
        return isfinite(ch_to_flonum(args[0])) ? CH_TRUE : CH_FALSE;
    }
    return ch_is_exact(args[0]) ? CH_TRUE : CH_FALSE;
}

static ChValue flonum_from_exact_int(ChValue v) {
    return ch_make_flonum(ch_exact_to_f64(v));
}

static ChValue prim_numerator(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    if (ch_is_exact_integer(args[0])) {
        return args[0];
    }
    if (ch_is_rational_obj(args[0])) {
        return ch_as_rational(args[0])->numerator;
    }
    if (ch_is_flonum(args[0])) {
        double f = ch_to_flonum(args[0]);
        if (!isfinite(f) || f == trunc(f)) {
            return args[0];
        }
        ChValue exact = ch_exact_from_flonum(&vm->gc, f);
        if (exact == CH_UNDEFINED) {
            snprintf(vm->error, sizeof(vm->error), "numerator: expected finite number");
            return CH_UNDEFINED;
        }
        ChValue num, den;
        ch_exact_parts(exact, &num, &den);
        return flonum_from_exact_int(num);
    }
    snprintf(vm->error, sizeof(vm->error), "numerator: expected exact number");
    return CH_UNDEFINED;
}

static ChValue prim_denominator(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    if (ch_is_exact_integer(args[0])) {
        return ch_make_fixnum(1);
    }
    if (ch_is_rational_obj(args[0])) {
        return ch_as_rational(args[0])->denominator;
    }
    if (ch_is_flonum(args[0])) {
        double f = ch_to_flonum(args[0]);
        if (!isfinite(f) || f == trunc(f)) {
            return ch_make_flonum(1.0);
        }
        ChValue exact = ch_exact_from_flonum(&vm->gc, f);
        if (exact == CH_UNDEFINED) {
            snprintf(vm->error, sizeof(vm->error), "denominator: expected finite number");
            return CH_UNDEFINED;
        }
        ChValue num, den;
        ch_exact_parts(exact, &num, &den);
        return flonum_from_exact_int(den);
    }
    snprintf(vm->error, sizeof(vm->error), "denominator: expected exact number");
    return CH_UNDEFINED;
}

static ChValue prim_procedure_p(ChVM *vm, ChValue *args, int nargs) {
    (void)vm;
    (void)nargs;
    return ch_is_procedure(args[0]) ? CH_TRUE : CH_FALSE;
}

static ChValue prim_eq_p(ChVM *vm, ChValue *args, int nargs) {
    (void)vm;
    (void)nargs;
    return ch_eq(args[0], args[1]) ? CH_TRUE : CH_FALSE;
}

static ChValue prim_eqv_p(ChVM *vm, ChValue *args, int nargs) {
    (void)vm;
    (void)nargs;
    return ch_eqv(args[0], args[1]) ? CH_TRUE : CH_FALSE;
}

static ChValue prim_equal_p(ChVM *vm, ChValue *args, int nargs) {
    (void)vm;
    (void)nargs;
    return ch_equal(args[0], args[1]) ? CH_TRUE : CH_FALSE;
}

static bool as_number(ChValue v, double *out) {
    if (ch_is_fixnum(v)) {
        *out = (double)ch_to_fixnum(v);
        return true;
    }
    if (ch_is_flonum(v)) {
        *out = ch_to_flonum(v);
        return true;
    }
    if (ch_is_bignum(v) || ch_is_rational_obj(v)) {
        *out = ch_exact_to_f64(v);
        return true;
    }
    return false;
}

static int all_exact(ChValue *args, int nargs) {
    for (int i = 0; i < nargs; i++) {
        if (!ch_is_exact(args[i])) {
            return 0;
        }
    }
    return 1;
}

static int any_complex(ChValue *args, int nargs) {
    for (int i = 0; i < nargs; i++) {
        if (ch_is_complex_obj(args[i])) {
            return 1;
        }
    }
    return 0;
}

static int any_inexact(ChValue *args, int nargs) {
    for (int i = 0; i < nargs; i++) {
        if (ch_is_flonum(args[i]) || ch_is_complex_obj(args[i])) {
            return 1;
        }
    }
    return 0;
}

static bool mixed_num_equal(ChGC *gc, ChValue a, ChValue b) {
    if (ch_is_flonum(a) && ch_is_exact(b)) {
        ChValue ea = ch_double_to_exact_if_exact(gc, ch_to_flonum(a));
        if (ea != CH_UNDEFINED) {
            return ch_exact_compare(gc, ea, b) == 0;
        }
        return false;
    }
    if (ch_is_flonum(b) && ch_is_exact(a)) {
        ChValue eb = ch_double_to_exact_if_exact(gc, ch_to_flonum(b));
        if (eb != CH_UNDEFINED) {
            return ch_exact_compare(gc, a, eb) == 0;
        }
        return false;
    }
    double da, db;
    return as_number(a, &da) && as_number(b, &db) && da == db;
}

typedef ChValue (*ChComplexBinOp)(ChGC *gc, ChValue a, ChValue b);

/* Fold args[start..] into acc with a complex binary op; roots acc across GC. */
static ChValue fold_complex(ChVM *vm, ChValue acc, ChValue *args, int start, int nargs,
                            ChComplexBinOp op, const char *err) {
    ch_gc_push(&vm->gc, &acc);
    for (int i = start; i < nargs; i++) {
        ChValue next = args[i];
        ch_gc_push(&vm->gc, &next);
        acc = op(&vm->gc, acc, next);
        ch_gc_pop(&vm->gc);
        if (acc == CH_UNDEFINED) {
            ch_gc_pop(&vm->gc);
            snprintf(vm->error, sizeof(vm->error), "%s", err);
            return CH_UNDEFINED;
        }
    }
    ch_gc_pop(&vm->gc);
    return acc;
}

static ChValue make_number(double d, bool all_fix) {
    if (all_fix) {
        return ch_make_fixnum((int64_t)d);
    }
    return ch_make_flonum(d);
}

static ChValue prim_add(ChVM *vm, ChValue *args, int nargs) {
    if (nargs > 0 && any_complex(args, nargs)) {
        return fold_complex(vm, ch_make_fixnum(0), args, 0, nargs, ch_complex_add, "+: not a number");
    }
    if (nargs > 0 && all_exact(args, nargs)) {
        ChValue acc = ch_make_fixnum(0);
        ch_gc_push(&vm->gc, &acc);
        for (int i = 0; i < nargs; i++) {
            ChValue next = args[i];
            ch_gc_push(&vm->gc, &next);
            acc = ch_exact_add(&vm->gc, acc, next);
            ch_gc_pop(&vm->gc);
        }
        ch_gc_pop(&vm->gc);
        return acc;
    }
    double sum = 0;
    bool all_fixnum = true;
    for (int i = 0; i < nargs; i++) {
        double x;
        if (!as_number(args[i], &x)) {
            snprintf(vm->error, sizeof(vm->error), "+: not a number");
            return CH_UNDEFINED;
        }
        if (!ch_is_fixnum(args[i])) {
            all_fixnum = false;
        }
        sum += x;
    }
    return make_number(sum, all_fixnum);
}

static ChValue prim_sub(ChVM *vm, ChValue *args, int nargs) {
    if (nargs == 0) {
        snprintf(vm->error, sizeof(vm->error), "-: needs at least one argument");
        return CH_UNDEFINED;
    }
    if (any_complex(args, nargs)) {
        if (nargs == 1) {
            ChValue r = ch_complex_negate(&vm->gc, args[0]);
            if (r == CH_UNDEFINED) {
                snprintf(vm->error, sizeof(vm->error), "-: not a number");
            }
            return r;
        }
        return fold_complex(vm, args[0], args, 1, nargs, ch_complex_sub, "-: not a number");
    }
    if (all_exact(args, nargs)) {
        if (nargs == 1) {
            return ch_exact_negate(&vm->gc, args[0]);
        }
        ChValue acc = args[0];
        ch_gc_push(&vm->gc, &acc);
        for (int i = 1; i < nargs; i++) {
            ChValue next = args[i];
            ch_gc_push(&vm->gc, &next);
            acc = ch_exact_sub(&vm->gc, acc, next);
            ch_gc_pop(&vm->gc);
        }
        ch_gc_pop(&vm->gc);
        return acc;
    }
    double x;
    if (!as_number(args[0], &x)) {
        snprintf(vm->error, sizeof(vm->error), "-: not a number");
        return CH_UNDEFINED;
    }
    bool all_fixnum = ch_is_fixnum(args[0]);
    if (nargs == 1) {
        return make_number(-x, all_fixnum);
    }
    for (int i = 1; i < nargs; i++) {
        double y;
        if (!as_number(args[i], &y)) {
            snprintf(vm->error, sizeof(vm->error), "-: not a number");
            return CH_UNDEFINED;
        }
        if (!ch_is_fixnum(args[i])) {
            all_fixnum = false;
        }
        x -= y;
    }
    return make_number(x, all_fixnum);
}

static ChValue prim_mul(ChVM *vm, ChValue *args, int nargs) {
    if (nargs > 0 && any_complex(args, nargs)) {
        return fold_complex(vm, ch_make_fixnum(1), args, 0, nargs, ch_complex_mul, "*: not a number");
    }
    if (nargs > 0 && all_exact(args, nargs)) {
        ChValue acc = ch_make_fixnum(1);
        ch_gc_push(&vm->gc, &acc);
        for (int i = 0; i < nargs; i++) {
            ChValue next = args[i];
            ch_gc_push(&vm->gc, &next);
            acc = ch_exact_mul(&vm->gc, acc, next);
            ch_gc_pop(&vm->gc);
        }
        ch_gc_pop(&vm->gc);
        return acc;
    }
    double prod = 1;
    bool all_fixnum = true;
    for (int i = 0; i < nargs; i++) {
        double x;
        if (!as_number(args[i], &x)) {
            snprintf(vm->error, sizeof(vm->error), "*: not a number");
            return CH_UNDEFINED;
        }
        if (!ch_is_fixnum(args[i])) {
            all_fixnum = false;
        }
        prod *= x;
    }
    return make_number(prod, all_fixnum);
}

static ChValue prim_div(ChVM *vm, ChValue *args, int nargs) {
    if (nargs == 0) {
        snprintf(vm->error, sizeof(vm->error), "/: needs at least one argument");
        return CH_UNDEFINED;
    }
    if (any_complex(args, nargs)) {
        ChValue acc;
        if (nargs == 1) {
            acc = ch_complex_div(&vm->gc, ch_make_fixnum(1), args[0]);
        } else {
            acc = fold_complex(vm, args[0], args, 1, nargs, ch_complex_div, "/: division by zero");
            if (acc == CH_UNDEFINED) {
                return CH_UNDEFINED; /* error already set */
            }
        }
        if (acc == CH_UNDEFINED) {
            snprintf(vm->error, sizeof(vm->error), "/: division by zero");
        }
        return acc;
    }
    if (all_exact(args, nargs)) {
        ChValue acc;
        if (nargs == 1) {
            acc = ch_exact_div(&vm->gc, ch_make_fixnum(1), args[0]);
        } else {
            acc = args[0];
            ch_gc_push(&vm->gc, &acc);
            for (int i = 1; i < nargs; i++) {
                ChValue next = args[i];
                ch_gc_push(&vm->gc, &next);
                acc = ch_exact_div(&vm->gc, acc, next);
                ch_gc_pop(&vm->gc);
                if (acc == CH_UNDEFINED) {
                    ch_gc_pop(&vm->gc);
                    snprintf(vm->error, sizeof(vm->error), "/: division by zero");
                    return CH_UNDEFINED;
                }
            }
            ch_gc_pop(&vm->gc);
        }
        if (acc == CH_UNDEFINED) {
            snprintf(vm->error, sizeof(vm->error), "/: division by zero");
        }
        return acc;
    }
    double x;
    if (!as_number(args[0], &x)) {
        snprintf(vm->error, sizeof(vm->error), "/: not a number");
        return CH_UNDEFINED;
    }
    if (nargs == 1) {
        return ch_make_flonum(1.0 / x);
    }
    for (int i = 1; i < nargs; i++) {
        double y;
        if (!as_number(args[i], &y)) {
            snprintf(vm->error, sizeof(vm->error), "/: not a number");
            return CH_UNDEFINED;
        }
        x /= y;
    }
    return ch_make_flonum(x);
}

static ChValue prim_num_eq(ChVM *vm, ChValue *args, int nargs) {
    if (nargs < 2) {
        return CH_TRUE;
    }
    if (any_complex(args, nargs)) {
        double r0, i0;
        if (!ch_complex_parts(args[0], &r0, &i0)) {
            snprintf(vm->error, sizeof(vm->error), "=: not a number");
            return CH_UNDEFINED;
        }
        for (int i = 1; i < nargs; i++) {
            double r, im;
            if (!ch_complex_parts(args[i], &r, &im)) {
                snprintf(vm->error, sizeof(vm->error), "=: not a number");
                return CH_UNDEFINED;
            }
            if (r0 != r || i0 != im) {
                return CH_FALSE;
            }
        }
        return CH_TRUE;
    }
    if (all_exact(args, nargs)) {
        for (int i = 1; i < nargs; i++) {
            if (ch_exact_compare(&vm->gc, args[0], args[i]) != 0) {
                return CH_FALSE;
            }
        }
        return CH_TRUE;
    }
    if (any_inexact(args, nargs)) {
        for (int i = 1; i < nargs; i++) {
            if (!mixed_num_equal(&vm->gc, args[0], args[i])) {
                return CH_FALSE;
            }
        }
        return CH_TRUE;
    }
    double first;
    if (!as_number(args[0], &first)) {
        snprintf(vm->error, sizeof(vm->error), "=: not a number");
        return CH_UNDEFINED;
    }
    for (int i = 1; i < nargs; i++) {
        double x;
        if (!as_number(args[i], &x)) {
            snprintf(vm->error, sizeof(vm->error), "=: not a number");
            return CH_UNDEFINED;
        }
        if (first != x) {
            return CH_FALSE;
        }
    }
    return CH_TRUE;
}

static int require_reals(ChVM *vm, ChValue *args, int nargs, const char *who) {
    if (any_complex(args, nargs)) {
        snprintf(vm->error, sizeof(vm->error), "%s: complex numbers not ordered", who);
        return 0;
    }
    return 1;
}

static ChValue prim_lt(ChVM *vm, ChValue *args, int nargs) {
    if (!require_reals(vm, args, nargs, "<")) {
        return CH_UNDEFINED;
    }
    if (nargs >= 2 && all_exact(args, nargs)) {
        for (int i = 0; i < nargs - 1; i++) {
            if (!(ch_exact_compare(&vm->gc, args[i], args[i + 1]) < 0)) {
                return CH_FALSE;
            }
        }
        return CH_TRUE;
    }
    for (int i = 0; i < nargs - 1; i++) {
        double a, b;
        if (!as_number(args[i], &a) || !as_number(args[i + 1], &b)) {
            snprintf(vm->error, sizeof(vm->error), "<: not a number");
            return CH_UNDEFINED;
        }
        if (!(a < b)) {
            return CH_FALSE;
        }
    }
    return CH_TRUE;
}

static ChValue prim_gt(ChVM *vm, ChValue *args, int nargs) {
    if (!require_reals(vm, args, nargs, ">")) {
        return CH_UNDEFINED;
    }
    if (nargs >= 2 && all_exact(args, nargs)) {
        for (int i = 0; i < nargs - 1; i++) {
            if (!(ch_exact_compare(&vm->gc, args[i], args[i + 1]) > 0)) {
                return CH_FALSE;
            }
        }
        return CH_TRUE;
    }
    for (int i = 0; i < nargs - 1; i++) {
        double a, b;
        if (!as_number(args[i], &a) || !as_number(args[i + 1], &b)) {
            snprintf(vm->error, sizeof(vm->error), ">: not a number");
            return CH_UNDEFINED;
        }
        if (!(a > b)) {
            return CH_FALSE;
        }
    }
    return CH_TRUE;
}

static ChValue prim_le(ChVM *vm, ChValue *args, int nargs) {
    if (!require_reals(vm, args, nargs, "<=")) {
        return CH_UNDEFINED;
    }
    if (nargs >= 2 && all_exact(args, nargs)) {
        for (int i = 0; i < nargs - 1; i++) {
            if (!(ch_exact_compare(&vm->gc, args[i], args[i + 1]) <= 0)) {
                return CH_FALSE;
            }
        }
        return CH_TRUE;
    }
    for (int i = 0; i < nargs - 1; i++) {
        double a, b;
        if (!as_number(args[i], &a) || !as_number(args[i + 1], &b)) {
            snprintf(vm->error, sizeof(vm->error), "<=: not a number");
            return CH_UNDEFINED;
        }
        if (!(a <= b)) {
            return CH_FALSE;
        }
    }
    return CH_TRUE;
}

static ChValue prim_ge(ChVM *vm, ChValue *args, int nargs) {
    if (!require_reals(vm, args, nargs, ">=")) {
        return CH_UNDEFINED;
    }
    if (nargs >= 2 && all_exact(args, nargs)) {
        for (int i = 0; i < nargs - 1; i++) {
            if (!(ch_exact_compare(&vm->gc, args[i], args[i + 1]) >= 0)) {
                return CH_FALSE;
            }
        }
        return CH_TRUE;
    }
    for (int i = 0; i < nargs - 1; i++) {
        double a, b;
        if (!as_number(args[i], &a) || !as_number(args[i + 1], &b)) {
            snprintf(vm->error, sizeof(vm->error), ">=: not a number");
            return CH_UNDEFINED;
        }
        if (!(a >= b)) {
            return CH_FALSE;
        }
    }
    return CH_TRUE;
}

static ChValue prim_quotient(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    if (!ch_is_exact_integer(args[0]) || !ch_is_exact_integer(args[1])) {
        snprintf(vm->error, sizeof(vm->error), "quotient: expected exact integers");
        return CH_UNDEFINED;
    }
    ChValue r = ch_bignum_quotient(&vm->gc, args[0], args[1]);
    if (r == CH_UNDEFINED) {
        snprintf(vm->error, sizeof(vm->error), "quotient: division by zero");
    }
    return r;
}

static ChValue prim_remainder(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    if (ch_is_flonum(args[0]) || ch_is_flonum(args[1])) {
        double a, b;
        if (!as_number(args[0], &a) || !as_number(args[1], &b)) {
            snprintf(vm->error, sizeof(vm->error), "remainder: not a number");
            return CH_UNDEFINED;
        }
        if (b == 0.0) {
            snprintf(vm->error, sizeof(vm->error), "remainder: division by zero");
            return CH_UNDEFINED;
        }
        return ch_make_flonum(fmod(a, b));
    }
    if (!ch_is_exact_integer(args[0]) || !ch_is_exact_integer(args[1])) {
        snprintf(vm->error, sizeof(vm->error), "remainder: expected exact integers");
        return CH_UNDEFINED;
    }
    ChValue r = ch_bignum_remainder(&vm->gc, args[0], args[1]);
    if (r == CH_UNDEFINED) {
        snprintf(vm->error, sizeof(vm->error), "remainder: division by zero");
    }
    return r;
}

static ChValue prim_not(ChVM *vm, ChValue *args, int nargs) {
    (void)vm;
    (void)nargs;
    return ch_is_true_value(args[0]) ? CH_FALSE : CH_TRUE;
}

static ChValue prim_display(ChVM *vm, ChValue *args, int nargs) {
    (void)vm;
    (void)nargs;
    ch_print_value(stdout, args[0], true);
    return CH_VOID;
}

static ChValue prim_newline(ChVM *vm, ChValue *args, int nargs) {
    (void)vm;
    (void)args;
    (void)nargs;
    fputc('\n', stdout);
    return CH_VOID;
}

static ChValue prim_write(ChVM *vm, ChValue *args, int nargs) {
    (void)vm;
    (void)nargs;
    ch_print_value(stdout, args[0], false);
    return CH_VOID;
}

static ChValue prim_list(ChVM *vm, ChValue *args, int nargs) {
    ChValue list = CH_NIL;
    ch_gc_push(&vm->gc, &list);
    for (int i = nargs - 1; i >= 0; i--) {
        ChValue item = args[i];
        ch_gc_push(&vm->gc, &item);
        list = ch_gc_cons(&vm->gc, item, list);
        ch_gc_pop(&vm->gc);
    }
    ch_gc_pop(&vm->gc);
    return list;
}

static ChValue prim_vector(ChVM *vm, ChValue *args, int nargs) {
    ChValue vec = ch_gc_make_vector(&vm->gc, (size_t)nargs, CH_FALSE);
    ChVector *v = ch_as_vector(vec);
    for (int i = 0; i < nargs; i++) {
        v->items[i] = args[i];
    }
    return vec;
}

static ChValue prim_vector_p(ChVM *vm, ChValue *args, int nargs) {
    (void)vm;
    (void)nargs;
    return ch_is_vector(args[0]) ? CH_TRUE : CH_FALSE;
}

static ChValue prim_boolean_p(ChVM *vm, ChValue *args, int nargs) {
    (void)vm;
    (void)nargs;
    return (args[0] == CH_TRUE || args[0] == CH_FALSE) ? CH_TRUE : CH_FALSE;
}

static ChValue prim_boolean_eq(ChVM *vm, ChValue *args, int nargs) {
    if (nargs == 0) {
        return CH_TRUE;
    }
    if (args[0] != CH_TRUE && args[0] != CH_FALSE) {
        snprintf(vm->error, sizeof(vm->error), "boolean=?: not a boolean");
        return CH_UNDEFINED;
    }
    for (int i = 1; i < nargs; i++) {
        if (args[i] != CH_TRUE && args[i] != CH_FALSE) {
            snprintf(vm->error, sizeof(vm->error), "boolean=?: not a boolean");
            return CH_UNDEFINED;
        }
        if (args[i] != args[0]) {
            return CH_FALSE;
        }
    }
    return CH_TRUE;
}

static ChValue prim_exit(ChVM *vm, ChValue *args, int nargs) {
    (void)vm;
    int code = 0;
    if (nargs >= 1) {
        if (ch_is_fixnum(args[0])) {
            code = (int)ch_to_fixnum(args[0]);
        } else if (args[0] == CH_FALSE) {
            code = 1;
        } else if (args[0] != CH_TRUE) {
            code = 1;
        }
    }
    exit(code);
    return CH_VOID; /* unreachable */
}

static ChValue prim_command_line(ChVM *vm, ChValue *args, int nargs) {
    (void)args;
    if (nargs != 0) {
        snprintf(vm->error, sizeof(vm->error), "command-line: expected 0 arguments");
        return CH_UNDEFINED;
    }
    ChValue list = CH_NIL;
    ch_gc_push(&vm->gc, &list);
    for (size_t i = vm->script_arg_count; i > 0; i--) {
        const char *a = vm->script_args[i - 1];
        ChValue s = ch_gc_make_string_cstr(&vm->gc, a ? a : "");
        list = ch_gc_cons(&vm->gc, s, list);
    }
    const char *prog = vm->script_path ? vm->script_path : "chaaya";
    ChValue ps = ch_gc_make_string_cstr(&vm->gc, prog);
    list = ch_gc_cons(&vm->gc, ps, list);
    ch_gc_pop(&vm->gc);
    return list;
}

static ChValue prim_make_rectangular(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    double re, im;
    if (!as_number(args[0], &re) || !as_number(args[1], &im) || ch_is_complex_obj(args[0]) ||
        ch_is_complex_obj(args[1])) {
        snprintf(vm->error, sizeof(vm->error), "make-rectangular: expected real numbers");
        return CH_UNDEFINED;
    }
    return ch_make_complex_ex(&vm->gc, re, im, ch_is_exact(args[0]), ch_is_exact(args[1]));
}

static ChValue prim_make_polar(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    double mag, ang;
    if (!as_number(args[0], &mag) || !as_number(args[1], &ang) || ch_is_complex_obj(args[0]) ||
        ch_is_complex_obj(args[1])) {
        snprintf(vm->error, sizeof(vm->error), "make-polar: expected real numbers");
        return CH_UNDEFINED;
    }
    return ch_make_complex(&vm->gc, mag * cos(ang), mag * sin(ang));
}

static int parts_or_error(ChVM *vm, ChValue v, double *re, double *im, const char *who) {
    if (!ch_complex_parts(v, re, im)) {
        snprintf(vm->error, sizeof(vm->error), "%s: not a number", who);
        return 0;
    }
    return 1;
}

static ChValue maybe_integral_flonum(ChGC *gc, double f) {
    if (isfinite(f) && f == trunc(f)) {
        ChValue e = ch_exact_from_flonum(gc, f);
        if (e != CH_UNDEFINED) {
            return e;
        }
    }
    return ch_make_flonum(f);
}

static ChValue prim_real_part(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    double re, im;
    if (!parts_or_error(vm, args[0], &re, &im, "real-part")) {
        return CH_UNDEFINED;
    }
    if (ch_is_exact(args[0]) && !ch_is_complex_obj(args[0])) {
        return args[0];
    }
    if (ch_is_complex_obj(args[0])) {
        ChComplex *c = ch_as_complex(args[0]);
        if (c->exact_real) {
            ChValue ex = ch_exact_from_flonum(&vm->gc, re);
            if (ex != CH_UNDEFINED) {
                return ex;
            }
        }
        return maybe_integral_flonum(&vm->gc, re);
    }
    return ch_make_flonum(re);
}

static ChValue prim_imag_part(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    double re, im;
    if (!parts_or_error(vm, args[0], &re, &im, "imag-part")) {
        return CH_UNDEFINED;
    }
    if (ch_is_exact(args[0]) && !ch_is_complex_obj(args[0])) {
        return ch_make_fixnum(0);
    }
    if (ch_is_complex_obj(args[0])) {
        ChComplex *c = ch_as_complex(args[0]);
        if (c->exact_imag) {
            ChValue ex = ch_exact_from_flonum(&vm->gc, im);
            if (ex != CH_UNDEFINED) {
                return ex;
            }
        }
        return maybe_integral_flonum(&vm->gc, im);
    }
    return ch_make_flonum(im);
}

static ChValue prim_magnitude(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    double re, im;
    if (!parts_or_error(vm, args[0], &re, &im, "magnitude")) {
        return CH_UNDEFINED;
    }
    if (im == 0.0 && ch_is_exact(args[0])) {
        return ch_exact_abs(&vm->gc, args[0]);
    }
    return ch_make_flonum(hypot(re, im));
}

static ChValue prim_angle(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    double re, im;
    if (!parts_or_error(vm, args[0], &re, &im, "angle")) {
        return CH_UNDEFINED;
    }
    return ch_make_flonum(atan2(im, re));
}

void ch_register_core_primitives(ChVM *vm) {
    define_prim(vm, "cons", prim_cons, 2, 2);
    define_prim(vm, "car", prim_car, 1, 1);
    define_prim(vm, "cdr", prim_cdr, 1, 1);
    define_prim(vm, "set-car!", prim_set_car, 2, 2);
    define_prim(vm, "set-cdr!", prim_set_cdr, 2, 2);
    define_prim(vm, "pair?", prim_pair_p, 1, 1);
    define_prim(vm, "null?", prim_null_p, 1, 1);
    define_prim(vm, "symbol?", prim_symbol_p, 1, 1);
    define_prim(vm, "string?", prim_string_p, 1, 1);
    define_prim(vm, "number?", prim_number_p, 1, 1);
    define_prim(vm, "complex?", prim_complex_p, 1, 1);
    define_prim(vm, "real?", prim_real_p, 1, 1);
    define_prim(vm, "exact?", prim_exact_p, 1, 1);
    define_prim(vm, "exact-integer?", prim_exact_integer_p, 1, 1);
    define_prim(vm, "inexact?", prim_inexact_p, 1, 1);
    define_prim(vm, "rational?", prim_rational_p, 1, 1);
    define_prim(vm, "numerator", prim_numerator, 1, 1);
    define_prim(vm, "denominator", prim_denominator, 1, 1);
    define_prim(vm, "make-rectangular", prim_make_rectangular, 2, 2);
    define_prim(vm, "make-polar", prim_make_polar, 2, 2);
    define_prim(vm, "real-part", prim_real_part, 1, 1);
    define_prim(vm, "imag-part", prim_imag_part, 1, 1);
    define_prim(vm, "magnitude", prim_magnitude, 1, 1);
    define_prim(vm, "angle", prim_angle, 1, 1);
    define_prim(vm, "procedure?", prim_procedure_p, 1, 1);
    define_prim(vm, "boolean?", prim_boolean_p, 1, 1);
    define_prim(vm, "boolean=?", prim_boolean_eq, -1, 0);
    define_prim(vm, "vector?", prim_vector_p, 1, 1);
    define_prim(vm, "eq?", prim_eq_p, 2, 2);
    define_prim(vm, "eqv?", prim_eqv_p, 2, 2);
    define_prim(vm, "equal?", prim_equal_p, 2, 2);
    define_prim(vm, "+", prim_add, -1, 0);
    define_prim(vm, "-", prim_sub, -1, 1);
    define_prim(vm, "*", prim_mul, -1, 0);
    define_prim(vm, "/", prim_div, -1, 1);
    define_prim(vm, "=", prim_num_eq, -1, 0);
    define_prim(vm, "<", prim_lt, -1, 2);
    define_prim(vm, ">", prim_gt, -1, 2);
    define_prim(vm, "<=", prim_le, -1, 2);
    define_prim(vm, ">=", prim_ge, -1, 2);
    define_prim(vm, "quotient", prim_quotient, 2, 2);
    define_prim(vm, "remainder", prim_remainder, 2, 2);
    define_prim(vm, "not", prim_not, 1, 1);
    define_prim(vm, "display", prim_display, 1, 1);
    define_prim(vm, "newline", prim_newline, 0, 0);
    define_prim(vm, "write", prim_write, 1, 1);
    define_prim(vm, "list", prim_list, -1, 0);
    define_prim(vm, "vector", prim_vector, -1, 0);
    define_prim(vm, "exit", prim_exit, -1, 0);
    define_prim(vm, "command-line", prim_command_line, 0, 0);
}
