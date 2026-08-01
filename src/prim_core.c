#include "chaaya/prim.h"

#include "chaaya/printer.h"

#include <stdio.h>
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
    ch_set_car(args[0], args[1]);
    return CH_VOID;
}

static ChValue prim_set_cdr(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    if (!ch_is_pair(args[0])) {
        snprintf(vm->error, sizeof(vm->error), "set-cdr!: not a pair");
        return CH_UNDEFINED;
    }
    ch_set_cdr(args[0], args[1]);
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
    return (ch_is_fixnum(args[0]) || ch_is_flonum(args[0])) ? CH_TRUE : CH_FALSE;
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
    return false;
}

static ChValue make_number(double d, bool all_fix) {
    if (all_fix) {
        return ch_make_fixnum((int64_t)d);
    }
    return ch_make_flonum(d);
}

static ChValue prim_add(ChVM *vm, ChValue *args, int nargs) {
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
    (void)vm;
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

static ChValue prim_lt(ChVM *vm, ChValue *args, int nargs) {
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
    define_prim(vm, "procedure?", prim_procedure_p, 1, 1);
    define_prim(vm, "boolean?", prim_boolean_p, 1, 1);
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
    define_prim(vm, "not", prim_not, 1, 1);
    define_prim(vm, "display", prim_display, 1, 1);
    define_prim(vm, "newline", prim_newline, 0, 0);
    define_prim(vm, "write", prim_write, 1, 1);
    define_prim(vm, "list", prim_list, -1, 0);
    define_prim(vm, "vector", prim_vector, -1, 0);
}
