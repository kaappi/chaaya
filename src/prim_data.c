#include "chaaya/prim.h"

#include "chaaya/bignum.h"
#include "chaaya/complex.h"
#include "chaaya/rational.h"

#include <stdio.h>

static void define_prim(ChVM *vm, const char *name, ChNativeFn fn, int arity, int min_arity) {
    ChValue sym = ch_gc_intern_symbol_cstr(&vm->gc, name);
    ChSymbol *s = ch_as_symbol(sym);
    int idx = ch_vm_intern_global(vm, s);
    ChValue nv = ch_gc_make_native(&vm->gc, fn, name, arity, min_arity);
    ch_vm_define_global(vm, idx, nv);
}

static ChValue prim_values(ChVM *vm, ChValue *args, int nargs) {
    if (nargs == 1) {
        return args[0];
    }
    return ch_gc_make_values(&vm->gc, args, (size_t)nargs);
}

static ChValue prim_call_with_values(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    ChValue producer = args[0];
    ChValue consumer = args[1];
    if (!ch_is_procedure(producer) || !ch_is_procedure(consumer)) {
        snprintf(vm->error, sizeof(vm->error), "call-with-values: not a procedure");
        return CH_UNDEFINED;
    }
    /* Capture before nested applies (producer may call values) clear the flag. */
    bool was_tail = vm->native_was_tail;
    ChValue produced = CH_VOID;
    ChVMStatus st = ch_vm_apply(vm, producer, NULL, 0, &produced);
    if (st == CH_VM_CONTINUATION_INVOKED) {
        vm->continuation_invoked = true;
        return CH_UNDEFINED;
    }
    if (st != CH_VM_OK) {
        return CH_UNDEFINED;
    }
    /* Producer delivered values (possibly via continuation barrier landing). */
    vm->continuation_invoked = false;

    ChValue cargs[CH_VM_MAX_PENDING_ARGS];
    int cnargs = 0;
    if (ch_is_values(produced)) {
        ChValues *vs = ch_as_values(produced);
        if (vs->count > (size_t)CH_VM_MAX_PENDING_ARGS) {
            snprintf(vm->error, sizeof(vm->error), "call-with-values: too many values");
            return CH_UNDEFINED;
        }
        cnargs = (int)vs->count;
        for (int i = 0; i < cnargs; i++) {
            cargs[i] = vs->items[i];
        }
    } else {
        cnargs = 1;
        cargs[0] = produced;
    }

    /* Tail position: request a follow-up call so the consumer is not nested
     * under another run_until (proper TCO for loop-cwv / let-values). */
    if (was_tail) {
        vm->has_pending_call = true;
        vm->pending_call_tail = true;
        vm->pending_proc = consumer;
        vm->pending_nargs = cnargs;
        for (int i = 0; i < cnargs; i++) {
            vm->pending_args[i] = cargs[i];
        }
        return CH_UNDEFINED;
    }

    ChValue result = CH_VOID;
    st = ch_vm_apply(vm, consumer, cargs, cnargs, &result);
    if (st == CH_VM_CONTINUATION_INVOKED) {
        vm->continuation_invoked = true;
        return CH_UNDEFINED;
    }
    if (st != CH_VM_OK) {
        return CH_UNDEFINED;
    }
    return ch_coerce_single(result);
}

static ChValue prim_abs(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    if (ch_is_exact(args[0])) {
        return ch_exact_abs(&vm->gc, args[0]);
    }
    if (ch_is_flonum(args[0])) {
        double d = ch_to_flonum(args[0]);
        return ch_make_flonum(d < 0 ? -d : d);
    }
    snprintf(vm->error, sizeof(vm->error), "abs: not a number");
    return CH_UNDEFINED;
}

static ChValue prim_integer_p(ChVM *vm, ChValue *args, int nargs) {
    (void)vm;
    (void)nargs;
    if (ch_is_exact_integer(args[0])) {
        return CH_TRUE;
    }
    if (ch_is_flonum(args[0])) {
        double d = ch_to_flonum(args[0]);
        return (d == (double)(int64_t)d) ? CH_TRUE : CH_FALSE;
    }
    return CH_FALSE;
}

static ChValue prim_zero_p(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    if (ch_is_exact_integer(args[0])) {
        return ch_bignum_compare(args[0], ch_make_fixnum(0)) == 0 ? CH_TRUE : CH_FALSE;
    }
    if (ch_is_rational_obj(args[0])) {
        return ch_bignum_compare(ch_as_rational(args[0])->numerator, ch_make_fixnum(0)) == 0
                   ? CH_TRUE
                   : CH_FALSE;
    }
    if (ch_is_flonum(args[0])) {
        return ch_to_flonum(args[0]) == 0.0 ? CH_TRUE : CH_FALSE;
    }
    if (ch_is_complex_obj(args[0])) {
        ChComplex *c = ch_as_complex(args[0]);
        return (c->real == 0.0 && c->imag == 0.0) ? CH_TRUE : CH_FALSE;
    }
    snprintf(vm->error, sizeof(vm->error), "zero?: not a number");
    return CH_UNDEFINED;
}

void ch_register_data_primitives(ChVM *vm) {
    define_prim(vm, "values", prim_values, -1, 0);
    define_prim(vm, "call-with-values", prim_call_with_values, 2, 2);
    define_prim(vm, "abs", prim_abs, 1, 1);
    define_prim(vm, "integer?", prim_integer_p, 1, 1);
    define_prim(vm, "zero?", prim_zero_p, 1, 1);
}
