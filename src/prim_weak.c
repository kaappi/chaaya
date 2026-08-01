#include "chaaya/prim.h"

#include <stdio.h>

static void define_prim(ChVM *vm, const char *name, ChNativeFn fn, int arity, int min_arity) {
    ChValue sym = ch_gc_intern_symbol_cstr(&vm->gc, name);
    int idx = ch_vm_intern_global(vm, ch_as_symbol(sym));
    ChValue nv = ch_gc_make_native(&vm->gc, fn, name, arity, min_arity);
    ch_vm_define_global(vm, idx, nv);
}

static ChEphemeron *require_ephemeron(ChVM *vm, ChValue v, const char *who) {
    if (!ch_is_ephemeron(v)) {
        snprintf(vm->error, sizeof(vm->error), "%s: expected ephemeron", who);
        return NULL;
    }
    return ch_as_ephemeron(v);
}

static ChValue prim_make_ephemeron(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    return ch_gc_make_ephemeron(&vm->gc, args[0], args[1]);
}

static ChValue prim_ephemeron_p(ChVM *vm, ChValue *args, int nargs) {
    (void)vm;
    (void)nargs;
    return ch_is_ephemeron(args[0]) ? CH_TRUE : CH_FALSE;
}

static ChValue prim_ephemeron_key(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    ChEphemeron *eph = require_ephemeron(vm, args[0], "ephemeron-key");
    if (!eph) {
        return CH_UNDEFINED;
    }
    return eph->key;
}

static ChValue prim_ephemeron_value(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    ChEphemeron *eph = require_ephemeron(vm, args[0], "ephemeron-value");
    if (!eph) {
        return CH_UNDEFINED;
    }
    return eph->value;
}

static ChValue prim_ephemeron_broken_p(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    ChEphemeron *eph = require_ephemeron(vm, args[0], "ephemeron-broken?");
    if (!eph) {
        return CH_UNDEFINED;
    }
    return eph->broken ? CH_TRUE : CH_FALSE;
}

static ChValue prim_ephemeron_ref(ChVM *vm, ChValue *args, int nargs) {
    if (nargs < 2 || nargs > 3) {
        snprintf(vm->error, sizeof(vm->error), "ephemeron-ref: expected 2 or 3 arguments");
        return CH_UNDEFINED;
    }
    ChEphemeron *eph = require_ephemeron(vm, args[0], "ephemeron-ref");
    if (!eph) {
        return CH_UNDEFINED;
    }
    ChValue fallback = nargs == 3 ? args[2] : CH_FALSE;
    if (eph->broken) {
        return fallback;
    }
    return ch_eq(eph->key, args[1]) ? eph->value : fallback;
}

static ChValue prim_reference_barrier(ChVM *vm, ChValue *args, int nargs) {
    (void)vm;
    (void)nargs;
    return args[0];
}

void ch_register_weak_primitives(ChVM *vm) {
    define_prim(vm, "make-ephemeron", prim_make_ephemeron, 2, 2);
    define_prim(vm, "ephemeron?", prim_ephemeron_p, 1, 1);
    define_prim(vm, "ephemeron-key", prim_ephemeron_key, 1, 1);
    define_prim(vm, "ephemeron-value", prim_ephemeron_value, 1, 1);
    define_prim(vm, "ephemeron-broken?", prim_ephemeron_broken_p, 1, 1);
    define_prim(vm, "ephemeron-ref", prim_ephemeron_ref, -1, 2);
    define_prim(vm, "reference-barrier", prim_reference_barrier, 1, 1);
}
