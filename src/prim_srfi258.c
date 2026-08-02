#include "chaaya/prim.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

static void define_prim(ChVM *vm, const char *name, ChNativeFn fn, int arity, int min_arity) {
    ChValue sym = ch_gc_intern_symbol_cstr(&vm->gc, name);
    int idx = ch_vm_intern_global(vm, ch_as_symbol(sym));
    ChValue nv = ch_gc_make_native(&vm->gc, fn, name, arity, min_arity);
    ch_vm_define_global(vm, idx, nv);
}

static _Atomic uint32_t gensym_counter = 0;

static ChValue prim_string_to_uninterned_symbol(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    if (!ch_is_string(args[0])) {
        snprintf(vm->error, sizeof(vm->error), "string->uninterned-symbol: expected string");
        return CH_UNDEFINED;
    }
    ChString *s = ch_as_string(args[0]);
    return ch_gc_alloc_uninterned_symbol(&vm->gc, s->data, s->len);
}

static ChValue prim_symbol_interned_p(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    if (!ch_is_symbol(args[0])) {
        snprintf(vm->error, sizeof(vm->error), "symbol-interned?: expected symbol");
        return CH_UNDEFINED;
    }
    return ch_symbol_is_interned(ch_as_symbol(args[0])) ? CH_TRUE : CH_FALSE;
}

static ChValue prim_generate_uninterned_symbol(ChVM *vm, ChValue *args, int nargs) {
    if (nargs > 1) {
        snprintf(vm->error, sizeof(vm->error),
                 "generate-uninterned-symbol: expected 0 or 1 arguments");
        return CH_UNDEFINED;
    }

    const char *prefix = "g";
    if (nargs == 1) {
        if (ch_is_string(args[0])) {
            prefix = ch_as_string(args[0])->data;
        } else if (ch_is_symbol(args[0])) {
            prefix = ch_as_symbol(args[0])->name;
        } else {
            snprintf(vm->error, sizeof(vm->error),
                     "generate-uninterned-symbol: expected string or symbol");
            return CH_UNDEFINED;
        }
    }

    uint32_t n = atomic_fetch_add_explicit(&gensym_counter, 1, memory_order_relaxed);
    char buf[256];
    int written = snprintf(buf, sizeof(buf), "%s%u", prefix, n);
    if (written < 0 || (size_t)written >= sizeof(buf)) {
        snprintf(vm->error, sizeof(vm->error), "generate-uninterned-symbol: name too long");
        return CH_UNDEFINED;
    }
    return ch_gc_alloc_uninterned_symbol(&vm->gc, buf, (size_t)written);
}

void ch_register_srfi258_primitives(ChVM *vm) {
    define_prim(vm, "string->uninterned-symbol", prim_string_to_uninterned_symbol, 1, 1);
    define_prim(vm, "symbol-interned?", prim_symbol_interned_p, 1, 1);
    define_prim(vm, "generate-uninterned-symbol", prim_generate_uninterned_symbol, -1, 0);
}
