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
    ch_gc_push(&vm->gc, &obj);

    for (;;) {
        if (!ch_is_promise(obj)) {
            ch_gc_pop(&vm->gc);
            return obj;
        }
        ChPromise *pr = ch_as_promise(obj);
        if (pr->forced) {
            obj = pr->value;
            continue;
        }
        if (!ch_is_procedure(pr->value)) {
            pr->forced = 1;
            pr->forcing = 0;
            ChValue v = pr->value;
            ch_gc_pop(&vm->gc);
            return v;
        }

        /* R7RS/SRFI-45: reentrant force of the same promise re-runs the thunk.
         * Do not reject when pr->forcing is already set. Cycle detection is
         * only when the *result* promise is already forcing. */
        pr->forcing = 1;
        ChValue thunk = pr->value;
        ChValue result = CH_UNDEFINED;
        ch_gc_push(&vm->gc, &thunk);
        ch_gc_push(&vm->gc, &result);
        if (ch_vm_apply(vm, thunk, NULL, 0, &result) != CH_VM_OK) {
            pr->forcing = 0;
            ch_gc_pop_n(&vm->gc, 3);
            return CH_UNDEFINED;
        }

        /* Another force may have completed this promise while we ran. */
        if (pr->forced) {
            pr->forcing = 0;
            obj = pr->value;
            ch_gc_pop_n(&vm->gc, 2);
            continue;
        }

        if (ch_is_promise(result)) {
            ChPromise *res_pr = ch_as_promise(result);
            if (res_pr->forcing) {
                snprintf(vm->error, sizeof(vm->error), "force: reentrant force");
                pr->forcing = 0;
                ch_gc_pop_n(&vm->gc, 3);
                return CH_UNDEFINED;
            }
            if (res_pr->forced) {
                pr->forcing = 0;
                pr->forced = 1;
                pr->value = res_pr->value;
                ch_gc_write_barrier(&vm->gc, &pr->header, pr->value);
                obj = res_pr->value;
                ch_gc_pop_n(&vm->gc, 2);
                continue;
            }
            /* delay-force merge: outer keeps forcing cleared; alias onto result. */
            pr->value = res_pr->value;
            ch_gc_write_barrier(&vm->gc, &pr->header, pr->value);
            res_pr->forced = 1;
            res_pr->value = obj;
            ch_gc_write_barrier(&vm->gc, &res_pr->header, res_pr->value);
            pr->forcing = 0;
            ch_gc_pop_n(&vm->gc, 2);
            continue;
        }

        pr->forced = 1;
        pr->forcing = 0;
        pr->value = result;
        ch_gc_write_barrier(&vm->gc, &pr->header, pr->value);
        ch_gc_pop_n(&vm->gc, 3);
        return result;
    }
}

void ch_register_lazy_primitives(ChVM *vm) {
    define_prim(vm, "promise?", prim_promise_p, 1, 1);
    define_prim(vm, "make-promise", prim_make_promise, 1, 1);
    define_prim(vm, "%make-promise", prim_make_promise_lazy, 1, 1);
    define_prim(vm, "force", prim_force, 1, 1);
}
