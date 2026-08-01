#include "chaaya/prim.h"

#include <stdio.h>

static void define_prim(ChVM *vm, const char *name, ChNativeFn fn, int arity, int min_arity) {
    ChValue sym = ch_gc_intern_symbol_cstr(&vm->gc, name);
    ChSymbol *s = ch_as_symbol(sym);
    int idx = ch_vm_intern_global(vm, s);
    ChValue nv = ch_gc_make_native(&vm->gc, fn, name, arity, min_arity);
    ch_vm_define_global(vm, idx, nv);
}

static ChValue build_irritants(ChVM *vm, ChValue *args, int start, int nargs) {
    ChValue irritants = CH_NIL;
    ch_gc_push(&vm->gc, &irritants);
    for (int i = nargs - 1; i >= start; i--) {
        ChValue item = args[i];
        ch_gc_push(&vm->gc, &item);
        irritants = ch_gc_cons(&vm->gc, item, irritants);
        ch_gc_pop(&vm->gc);
    }
    ch_gc_pop(&vm->gc);
    return irritants;
}

static ChValue prim_make_error(ChVM *vm, ChValue *args, int nargs) {
    ChValue message = args[0];
    ChValue irritants = build_irritants(vm, args, 1, nargs);
    return ch_gc_make_error_object(&vm->gc, message, irritants, 0);
}

static ChValue prim_error(ChVM *vm, ChValue *args, int nargs) {
    ChValue err = prim_make_error(vm, args, nargs);
    return ch_vm_raise(vm, err, 0);
}

static ChValue prim_error_p(ChVM *vm, ChValue *args, int nargs) {
    (void)vm;
    (void)nargs;
    return ch_is_error_object(args[0]) ? CH_TRUE : CH_FALSE;
}

static ChValue prim_error_object_p(ChVM *vm, ChValue *args, int nargs) {
    (void)vm;
    (void)nargs;
    return ch_is_error_object(args[0]) ? CH_TRUE : CH_FALSE;
}

static ChValue prim_error_object_message(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    if (!ch_is_error_object(args[0])) {
        snprintf(vm->error, sizeof(vm->error), "error-object-message: expected error object");
        return CH_UNDEFINED;
    }
    return ch_as_error_object(args[0])->message;
}

static ChValue prim_error_object_irritants(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    if (!ch_is_error_object(args[0])) {
        snprintf(vm->error, sizeof(vm->error), "error-object-irritants: expected error object");
        return CH_UNDEFINED;
    }
    return ch_as_error_object(args[0])->irritants;
}

static ChValue prim_file_error_p(ChVM *vm, ChValue *args, int nargs) {
    (void)vm;
    (void)nargs;
    if (!ch_is_error_object(args[0])) {
        return CH_FALSE;
    }
    return ch_as_error_object(args[0])->error_type == 1 ? CH_TRUE : CH_FALSE;
}

static ChValue prim_read_error_p(ChVM *vm, ChValue *args, int nargs) {
    (void)vm;
    (void)nargs;
    if (!ch_is_error_object(args[0])) {
        return CH_FALSE;
    }
    return ch_as_error_object(args[0])->error_type == 2 ? CH_TRUE : CH_FALSE;
}

void ch_register_error_primitives(ChVM *vm) {
    define_prim(vm, "make-error", prim_make_error, -1, 1);
    define_prim(vm, "error", prim_error, -1, 1);
    define_prim(vm, "error?", prim_error_p, 1, 1);
    define_prim(vm, "error-object?", prim_error_object_p, 1, 1);
    define_prim(vm, "error-object-message", prim_error_object_message, 1, 1);
    define_prim(vm, "error-object-irritants", prim_error_object_irritants, 1, 1);
    define_prim(vm, "file-error?", prim_file_error_p, 1, 1);
    define_prim(vm, "read-error?", prim_read_error_p, 1, 1);
}
