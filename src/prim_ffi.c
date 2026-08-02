#include "chaaya/prim.h"

#include "chaaya/bignum.h"
#include "chaaya/ffi.h"
#include "chaaya/ffi_callback.h"
#include "chaaya/sandbox.h"

#include <stdint.h>
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
    if (ch_ffi_check_sandbox(vm, "open-foreign-library") != 0) {
        return CH_UNDEFINED;
    }

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
                     "foreign-procedure: supports at most %d arguments", CH_FFI_MAX_ARGS);
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

static void retarget_error_prefix(ChVM *vm, const char *from, const char *to) {
    size_t from_len = strlen(from);
    if (strncmp(vm->error, from, from_len) == 0) {
        char buf[512];
        snprintf(buf, sizeof(buf), "%s%s", to, vm->error + from_len);
        snprintf(vm->error, sizeof(vm->error), "%s", buf);
    }
}

static ChValue prim_ffi_open(ChVM *vm, ChValue *args, int nargs) {
    ChValue result = prim_open_foreign_library(vm, args, nargs);
    if (result == CH_UNDEFINED && vm->error[0]) {
        retarget_error_prefix(vm, "open-foreign-library:", "ffi-open:");
    }
    return result;
}

static ChValue prim_ffi_fn(ChVM *vm, ChValue *args, int nargs) {
    ChValue result = prim_foreign_procedure(vm, args, nargs);
    if (result == CH_UNDEFINED && vm->error[0]) {
        retarget_error_prefix(vm, "foreign-procedure:", "ffi-fn:");
    }
    return result;
}

static int exact_to_u64(ChValue value, uint64_t *out) {
    if (ch_is_fixnum(value)) {
        int64_t n = ch_to_fixnum(value);
        if (n < 0) {
            return 0;
        }
        *out = (uint64_t)n;
        return 1;
    }
    if (ch_is_bignum(value)) {
        ChBignum *bn = ch_as_bignum(value);
        if (!bn->positive || bn->len != 1) {
            return 0;
        }
        *out = bn->limbs[0];
        return 1;
    }
    return 0;
}

static void *value_to_fn_ptr(ChVM *vm, ChValue value) {
    (void)vm;
    if (value == CH_FALSE) {
        return NULL;
    }
    uint64_t addr = 0;
    if (!exact_to_u64(value, &addr)) {
        return NULL;
    }
    return (void *)(uintptr_t)addr;
}

static ChValue fn_ptr_to_value(ChVM *vm, void *ptr) {
    if (!ptr) {
        return CH_FALSE;
    }
    uintptr_t addr = (uintptr_t)ptr;
    if (addr <= (uintptr_t)INT64_MAX) {
        return ch_make_integer(&vm->gc, (int64_t)addr);
    }
    uint64_t limbs[1] = {(uint64_t)addr};
    return ch_gc_make_bignum_from_limbs(&vm->gc, limbs, 1, 1);
}

static ChValue prim_ffi_callback(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    if (ch_ffi_check_sandbox(vm, "ffi-callback") != 0) {
        return CH_UNDEFINED;
    }
    if (!ch_is_procedure(args[0])) {
        snprintf(vm->error, sizeof(vm->error), "ffi-callback: expected procedure");
        return CH_UNDEFINED;
    }

    ChFFIType arg_types[CH_FFI_MAX_ARGS];
    uint8_t arity = 0;
    if (parse_arg_types(vm, args[1], arg_types, &arity) != 0) {
        retarget_error_prefix(vm, "foreign-procedure:", "ffi-callback:");
        return CH_UNDEFINED;
    }

    ChFFIType result_type = CH_FFI_TYPE_VOID;
    if (!ch_ffi_parse_type_symbol(args[2], &result_type)) {
        snprintf(vm->error, sizeof(vm->error), "ffi-callback: unsupported return type");
        return CH_UNDEFINED;
    }

    ChFFICallbackSig sig;
    if (!ch_ffi_callback_match_sig(arg_types, arity, result_type, &sig)) {
        snprintf(vm->error, sizeof(vm->error), "ffi-callback: unsupported callback signature");
        return CH_UNDEFINED;
    }

    char err[256] = {0};
    void *fn = ch_ffi_callback_make_sig(vm, args[0], sig, err, sizeof(err));
    if (!fn) {
        snprintf(vm->error, sizeof(vm->error), "%s", err[0] ? err : "ffi-callback: allocation failed");
        return CH_UNDEFINED;
    }
    return fn_ptr_to_value(vm, fn);
}

static ChValue prim_ffi_callback_release(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    void *fn = value_to_fn_ptr(vm, args[0]);
    if (!fn || !ch_ffi_callback_p(fn)) {
        snprintf(vm->error, sizeof(vm->error), "ffi-callback-release: expected ffi-callback");
        return CH_UNDEFINED;
    }
    ch_ffi_callback_release(fn);
    return CH_VOID;
}

static ChValue prim_ffi_callback_p(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    void *fn = value_to_fn_ptr(vm, args[0]);
    return (fn && ch_ffi_callback_p(fn)) ? CH_TRUE : CH_FALSE;
}

/* (ffi-bytevector-ptr bv) => exact integer address of the bytevector's
 * backing storage, for handing to FFI calls that expect a raw buffer
 * pointer (e.g. as a `pointer` argument alongside a separate length). */
static ChValue prim_ffi_bytevector_ptr(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    if (!ch_is_bytevector(args[0])) {
        snprintf(vm->error, sizeof(vm->error), "ffi-bytevector-ptr: expected bytevector");
        return CH_UNDEFINED;
    }
    return fn_ptr_to_value(vm, ch_as_bytevector(args[0])->data);
}

void ch_register_ffi_primitives(ChVM *vm) {
    define_prim(vm, "open-foreign-library", prim_open_foreign_library, 1, 1);
    define_prim(vm, "close-foreign-library!", prim_close_foreign_library, 1, 1);
    define_prim(vm, "foreign-library?", prim_foreign_library_p, 1, 1);
    define_prim(vm, "foreign-procedure", prim_foreign_procedure, 4, 4);
    define_prim(vm, "foreign-procedure?", prim_foreign_procedure_p, 1, 1);

    /* Compatibility aliases used by Kaappi smoke tests. */
    define_prim(vm, "ffi-open", prim_ffi_open, 1, 1);
    define_prim(vm, "ffi-close", prim_close_foreign_library, 1, 1);
    define_prim(vm, "ffi-fn", prim_ffi_fn, 4, 4);
    define_prim(vm, "ffi-callback", prim_ffi_callback, 3, 3);
    define_prim(vm, "ffi-callback-release", prim_ffi_callback_release, 1, 1);
    define_prim(vm, "ffi-callback?", prim_ffi_callback_p, 1, 1);
    define_prim(vm, "ffi-bytevector-ptr", prim_ffi_bytevector_ptr, 1, 1);
}
