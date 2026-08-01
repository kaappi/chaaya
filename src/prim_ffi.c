#include "chaaya/prim.h"

#include "chaaya/ffi.h"

#include <stdio.h>
#include <string.h>

static void define_prim(ChVM *vm, const char *name, ChNativeFn fn, int arity, int min_arity) {
    ChValue sym = ch_gc_intern_symbol_cstr(&vm->gc, name);
    int idx = ch_vm_intern_global(vm, ch_as_symbol(sym));
    ChValue nv = ch_gc_make_native(&vm->gc, fn, name, arity, min_arity);
    ch_vm_define_global(vm, idx, nv);
}

static ChValue prim_open_foreign_library(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    const char *path = NULL;
    if (args[0] != CH_FALSE) {
        if (!ch_is_string(args[0])) {
            snprintf(vm->error, sizeof(vm->error),
                     "open-foreign-library: expected string path or #f");
            return CH_UNDEFINED;
        }
        path = ch_as_string(args[0])->data;
    }
    ChValue lib = CH_NIL;
    if (ch_ffi_open_library(vm, path, &lib) != 0) {
        return CH_UNDEFINED;
    }
    return lib;
}

static ChValue prim_close_foreign_library(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    if (ch_ffi_close_library(vm, args[0]) != 0) {
        return CH_UNDEFINED;
    }
    return CH_VOID;
}

static ChValue prim_foreign_library_p(ChVM *vm, ChValue *args, int nargs) {
    (void)vm;
    (void)nargs;
    return ch_is_foreign_library(args[0]) ? CH_TRUE : CH_FALSE;
}

static ChValue prim_foreign_procedure_p(ChVM *vm, ChValue *args, int nargs) {
    (void)vm;
    (void)nargs;
    return ch_is_foreign_procedure(args[0]) ? CH_TRUE : CH_FALSE;
}

static int parse_arg_types(ChVM *vm, ChValue list, ChFFIType *types, uint8_t *arity) {
    uint8_t count = 0;
    ChValue it = list;
    while (ch_is_pair(it)) {
        if (count >= CH_FFI_MAX_ARGS) {
            snprintf(vm->error, sizeof(vm->error),
                     "foreign-procedure: MVP supports at most %d arguments", CH_FFI_MAX_ARGS);
            return -1;
        }
        ChValue type_sym = ch_car(it);
        if (!ch_ffi_parse_type_symbol(type_sym, &types[count])) {
            snprintf(vm->error, sizeof(vm->error),
                     "foreign-procedure: unsupported argument type");
            return -1;
        }
        if (types[count] == CH_FFI_TYPE_VOID) {
            snprintf(vm->error, sizeof(vm->error),
                     "foreign-procedure: void is not a valid argument type");
            return -1;
        }
        count++;
        it = ch_cdr(it);
    }
    if (!ch_is_nil(it)) {
        snprintf(vm->error, sizeof(vm->error),
                 "foreign-procedure: argument type list must be a proper list");
        return -1;
    }
    *arity = count;
    return 0;
}

static ChValue prim_foreign_procedure(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    if (!ch_is_foreign_library(args[0])) {
        snprintf(vm->error, sizeof(vm->error),
                 "foreign-procedure: expected foreign library handle");
        return CH_UNDEFINED;
    }

    const char *symbol_name = NULL;
    if (ch_is_string(args[1])) {
        symbol_name = ch_as_string(args[1])->data;
    } else if (ch_is_symbol(args[1])) {
        symbol_name = ch_as_symbol(args[1])->name;
    } else {
        snprintf(vm->error, sizeof(vm->error),
                 "foreign-procedure: expected symbol name as string or symbol");
        return CH_UNDEFINED;
    }

    ChFFIType arg_types[CH_FFI_MAX_ARGS];
    uint8_t arity = 0;
    if (parse_arg_types(vm, args[2], arg_types, &arity) != 0) {
        return CH_UNDEFINED;
    }

    ChFFIType result_type = CH_FFI_TYPE_VOID;
    if (!ch_ffi_parse_type_symbol(args[3], &result_type)) {
        snprintf(vm->error, sizeof(vm->error),
                 "foreign-procedure: unsupported return type");
        return CH_UNDEFINED;
    }

    ChValue proc = CH_NIL;
    if (ch_ffi_lookup_procedure(vm, args[0], symbol_name, result_type, arg_types, arity, &proc) != 0) {
        return CH_UNDEFINED;
    }
    return proc;
}

void ch_register_ffi_primitives(ChVM *vm) {
    define_prim(vm, "open-foreign-library", prim_open_foreign_library, 1, 1);
    define_prim(vm, "close-foreign-library!", prim_close_foreign_library, 1, 1);
    define_prim(vm, "foreign-library?", prim_foreign_library_p, 1, 1);
    define_prim(vm, "foreign-procedure", prim_foreign_procedure, 4, 4);
    define_prim(vm, "foreign-procedure?", prim_foreign_procedure_p, 1, 1);

    /* Compatibility aliases used by Kaappi smoke tests. */
    define_prim(vm, "ffi-open", prim_open_foreign_library, 1, 1);
    define_prim(vm, "ffi-close", prim_close_foreign_library, 1, 1);
    define_prim(vm, "ffi-fn", prim_foreign_procedure, 4, 4);
}
