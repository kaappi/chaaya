#include "chaaya/prim.h"

#include <stdio.h>

static void define_prim(ChVM *vm, const char *name, ChNativeFn fn, int arity, int min_arity) {
    ChValue sym = ch_gc_intern_symbol_cstr(&vm->gc, name);
    int idx = ch_vm_intern_global(vm, ch_as_symbol(sym));
    ChValue nv = ch_gc_make_native(&vm->gc, fn, name, arity, min_arity);
    ch_vm_define_global(vm, idx, nv);
}

static ChValue prim_promise_p(ChVM *vm, ChValue *args, int nargs) {
    (void)vm;
    (void)nargs;
    return ch_is_promise(args[0]) ? CH_TRUE : CH_FALSE;
}

static ChValue prim_make_promise(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    if (ch_is_promise(args[0])) {
        return args[0];
    }
    return ch_gc_make_promise(&vm->gc, 1, args[0]);
}

static ChValue prim_make_promise_lazy(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    return ch_gc_make_promise(&vm->gc, 0, args[0]);
}

static ChValue prim_force(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    ChValue obj = args[0];
    if (!ch_is_promise(obj)) {
        return obj;
    }
    ChPromise *pr = ch_as_promise(obj);
    if (pr->forced) {
        return pr->value;
    }
    if (pr->forcing) {
        snprintf(vm->error, sizeof(vm->error), "force: reentrant force");
        return CH_UNDEFINED;
    }
    if (!ch_is_procedure(pr->value)) {
        snprintf(vm->error, sizeof(vm->error), "force: promise value is not a thunk");
        return CH_UNDEFINED;
    }
    pr->forcing = 1;
    ChValue thunk = pr->value;
    ChValue result = CH_UNDEFINED;
    ch_gc_push(&vm->gc, &obj);
    ch_gc_push(&vm->gc, &thunk);
    ch_gc_push(&vm->gc, &result);
    if (ch_vm_apply(vm, thunk, NULL, 0, &result) != CH_VM_OK) {
        pr->forcing = 0;
        ch_gc_pop_n(&vm->gc, 3);
        return CH_UNDEFINED;
    }
    /* If force produced another promise, force it (R7RS delay-force style). */
    while (ch_is_promise(result) && !ch_as_promise(result)->forced) {
        ChPromise *inner = ch_as_promise(result);
        if (inner->forcing) {
            snprintf(vm->error, sizeof(vm->error), "force: reentrant force");
            pr->forcing = 0;
            ch_gc_pop_n(&vm->gc, 3);
            return CH_UNDEFINED;
        }
        if (!ch_is_procedure(inner->value)) {
            result = inner->value;
            break;
        }
        inner->forcing = 1;
        ChValue t2 = inner->value;
        ChValue r2 = CH_UNDEFINED;
        ch_gc_push(&vm->gc, &t2);
        ch_gc_push(&vm->gc, &r2);
        if (ch_vm_apply(vm, t2, NULL, 0, &r2) != CH_VM_OK) {
            inner->forcing = 0;
            pr->forcing = 0;
            ch_gc_pop_n(&vm->gc, 5);
            return CH_UNDEFINED;
        }
        inner->forced = 1;
        inner->forcing = 0;
        inner->value = r2;
        result = r2;
        ch_gc_pop_n(&vm->gc, 2);
    }
    if (ch_is_promise(result) && ch_as_promise(result)->forced) {
        result = ch_as_promise(result)->value;
    }
    pr->forced = 1;
    pr->forcing = 0;
    pr->value = result;
    ch_gc_pop_n(&vm->gc, 3);
    return result;
}

void ch_register_lazy_primitives(ChVM *vm) {
    define_prim(vm, "promise?", prim_promise_p, 1, 1);
    define_prim(vm, "make-promise", prim_make_promise, 1, 1);
    define_prim(vm, "%make-promise", prim_make_promise_lazy, 1, 1);
    define_prim(vm, "force", prim_force, 1, 1);
}
