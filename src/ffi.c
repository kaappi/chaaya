#include "chaaya/ffi.h"

#include "chaaya/bignum.h"
#include "chaaya/rational.h"
#include "chaaya/vm.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

typedef union ChFFIWord {
    int64_t i;
    double d;
    void *p;
} ChFFIWord;

static void set_error(ChVM *vm, const char *msg) {
    snprintf(vm->error, sizeof(vm->error), "%s", msg);
}

const char *ch_ffi_type_name(ChFFIType type) {
    switch (type) {
    case CH_FFI_TYPE_VOID:
        return "void";
    case CH_FFI_TYPE_INT:
        return "int";
    case CH_FFI_TYPE_DOUBLE:
        return "double";
    case CH_FFI_TYPE_POINTER:
        return "pointer";
    case CH_FFI_TYPE_FLOAT:
        return "float";
    case CH_FFI_TYPE_BOOL:
        return "bool";
    case CH_FFI_TYPE_INT8:
        return "int8";
    case CH_FFI_TYPE_UINT8:
        return "uint8";
    case CH_FFI_TYPE_INT16:
        return "int16";
    case CH_FFI_TYPE_UINT16:
        return "uint16";
    case CH_FFI_TYPE_INT32:
        return "int32";
    case CH_FFI_TYPE_UINT32:
        return "uint32";
    case CH_FFI_TYPE_INT64:
        return "int64";
    case CH_FFI_TYPE_UINT64:
        return "uint64";
    case CH_FFI_TYPE_SIZE:
        return "size_t";
    case CH_FFI_TYPE_LONG:
        return "long";
    }
    return "unknown";
}

int ch_ffi_parse_type_symbol(ChValue sym, ChFFIType *out_type) {
    if (!ch_is_symbol(sym) || !out_type) {
        return 0;
    }
    const char *name = ch_symbol_basename(ch_as_symbol(sym));
    if (strcmp(name, "void") == 0) {
        *out_type = CH_FFI_TYPE_VOID;
        return 1;
    }
    if (strcmp(name, "int") == 0 || strcmp(name, "integer") == 0 || strcmp(name, "c-int") == 0) {
        *out_type = CH_FFI_TYPE_INT;
        return 1;
    }
    if (strcmp(name, "long") == 0) {
        *out_type = CH_FFI_TYPE_LONG;
        return 1;
    }
    if (strcmp(name, "long-long") == 0 || strcmp(name, "int64") == 0) {
        *out_type = CH_FFI_TYPE_INT64;
        return 1;
    }
    if (strcmp(name, "int8") == 0) {
        *out_type = CH_FFI_TYPE_INT8;
        return 1;
    }
    if (strcmp(name, "uint8") == 0 || strcmp(name, "char") == 0) {
        *out_type = CH_FFI_TYPE_UINT8;
        return 1;
    }
    if (strcmp(name, "int16") == 0) {
        *out_type = CH_FFI_TYPE_INT16;
        return 1;
    }
    if (strcmp(name, "uint16") == 0) {
        *out_type = CH_FFI_TYPE_UINT16;
        return 1;
    }
    if (strcmp(name, "int32") == 0) {
        *out_type = CH_FFI_TYPE_INT32;
        return 1;
    }
    if (strcmp(name, "uint32") == 0) {
        *out_type = CH_FFI_TYPE_UINT32;
        return 1;
    }
    if (strcmp(name, "uint64") == 0) {
        *out_type = CH_FFI_TYPE_UINT64;
        return 1;
    }
    if (strcmp(name, "size") == 0 || strcmp(name, "size_t") == 0) {
        *out_type = CH_FFI_TYPE_SIZE;
        return 1;
    }
    if (strcmp(name, "double") == 0) {
        *out_type = CH_FFI_TYPE_DOUBLE;
        return 1;
    }
    if (strcmp(name, "float") == 0) {
        *out_type = CH_FFI_TYPE_FLOAT;
        return 1;
    }
    if (strcmp(name, "bool") == 0 || strcmp(name, "boolean") == 0) {
        *out_type = CH_FFI_TYPE_BOOL;
        return 1;
    }
    if (strcmp(name, "pointer") == 0 || strcmp(name, "ptr") == 0 || strcmp(name, "string") == 0 ||
        strcmp(name, "c-string") == 0) {
        *out_type = CH_FFI_TYPE_POINTER;
        return 1;
    }
    return 0;
}

typedef enum ChFFICallClass {
    CH_FFI_CLASS_INT = 0,
    CH_FFI_CLASS_DOUBLE = 1,
    CH_FFI_CLASS_POINTER = 2,
} ChFFICallClass;

static ChFFICallClass ffi_call_class(ChFFIType type) {
    switch (type) {
    case CH_FFI_TYPE_VOID:
    case CH_FFI_TYPE_INT:
    case CH_FFI_TYPE_BOOL:
    case CH_FFI_TYPE_INT8:
    case CH_FFI_TYPE_UINT8:
    case CH_FFI_TYPE_INT16:
    case CH_FFI_TYPE_UINT16:
    case CH_FFI_TYPE_INT32:
    case CH_FFI_TYPE_UINT32:
    case CH_FFI_TYPE_INT64:
    case CH_FFI_TYPE_UINT64:
    case CH_FFI_TYPE_SIZE:
    case CH_FFI_TYPE_LONG:
        return CH_FFI_CLASS_INT;
    case CH_FFI_TYPE_FLOAT:
        return CH_FFI_CLASS_DOUBLE;
    case CH_FFI_TYPE_DOUBLE:
        return CH_FFI_CLASS_DOUBLE;
    case CH_FFI_TYPE_POINTER:
        return CH_FFI_CLASS_POINTER;
    }
    return CH_FFI_CLASS_INT;
}

static int store_int_result(ChFFIType result_type, int64_t raw, ChFFIWord *out_word) {
    switch (result_type) {
    case CH_FFI_TYPE_VOID:
        return 0;
    case CH_FFI_TYPE_POINTER:
        out_word->p = (void *)(uintptr_t)raw;
        return 0;
    case CH_FFI_TYPE_BOOL:
        out_word->i = raw ? 1 : 0;
        return 0;
    case CH_FFI_TYPE_INT:
    case CH_FFI_TYPE_INT8:
    case CH_FFI_TYPE_UINT8:
    case CH_FFI_TYPE_INT16:
    case CH_FFI_TYPE_UINT16:
    case CH_FFI_TYPE_INT32:
    case CH_FFI_TYPE_UINT32:
    case CH_FFI_TYPE_INT64:
    case CH_FFI_TYPE_UINT64:
    case CH_FFI_TYPE_SIZE:
    case CH_FFI_TYPE_LONG:
        out_word->i = raw;
        return 0;
    case CH_FFI_TYPE_FLOAT:
    case CH_FFI_TYPE_DOUBLE:
        out_word->d = (double)raw;
        return 0;
    }
    return -1;
}

static int exact_to_i64(ChValue value, int64_t *out) {
    if (ch_is_fixnum(value)) {
        *out = ch_to_fixnum(value);
        return 1;
    }
    if (ch_is_bignum(value)) {
        double d = ch_bignum_to_f64(value);
        if (!isfinite(d) || d < (double)INT64_MIN || d > (double)INT64_MAX || floor(d) != d) {
            return 0;
        }
        *out = (int64_t)d;
        return 1;
    }
    return 0;
}

static int marshal_int_arg(ChVM *vm, ChValue value, int64_t *out) {
    if (value == CH_TRUE) {
        *out = 1;
        return 0;
    }
    if (value == CH_FALSE) {
        *out = 0;
        return 0;
    }
    if (!exact_to_i64(value, out)) {
        set_error(vm, "ffi: expected exact integer argument");
        return -1;
    }
    return 0;
}

static int marshal_bool_arg(ChVM *vm, ChValue value, int64_t *out) {
    if (value == CH_TRUE) {
        *out = 1;
        return 0;
    }
    if (value == CH_FALSE) {
        *out = 0;
        return 0;
    }
    set_error(vm, "ffi: expected boolean argument");
    return -1;
}

static int marshal_double_arg(ChVM *vm, ChValue value, double *out) {
    if (ch_is_flonum(value)) {
        *out = ch_to_flonum(value);
        return 0;
    }
    if (ch_is_fixnum(value)) {
        *out = (double)ch_to_fixnum(value);
        return 0;
    }
    if (ch_is_exact(value)) {
        *out = ch_exact_to_f64(value);
        return 0;
    }
    set_error(vm, "ffi: expected numeric argument");
    return -1;
}

static int marshal_pointer_arg(ChVM *vm, ChValue value, void **out) {
    if (value == CH_FALSE) {
        *out = NULL;
        return 0;
    }
    if (ch_is_string(value)) {
        *out = ch_as_string(value)->data;
        return 0;
    }
    if (ch_is_bytevector(value)) {
        *out = ch_as_bytevector(value)->data;
        return 0;
    }
    int64_t as_i64 = 0;
    if (exact_to_i64(value, &as_i64)) {
        *out = (void *)(uintptr_t)as_i64;
        return 0;
    }
    set_error(vm, "ffi: expected pointer-compatible argument (#f, exact integer, string, or bytevector)");
    return -1;
}

static int marshal_arg(ChVM *vm, ChFFIType type, ChValue value, ChFFIWord *out) {
    switch (type) {
    case CH_FFI_TYPE_INT:
    case CH_FFI_TYPE_INT8:
    case CH_FFI_TYPE_UINT8:
    case CH_FFI_TYPE_INT16:
    case CH_FFI_TYPE_UINT16:
    case CH_FFI_TYPE_INT32:
    case CH_FFI_TYPE_UINT32:
    case CH_FFI_TYPE_INT64:
    case CH_FFI_TYPE_UINT64:
    case CH_FFI_TYPE_SIZE:
    case CH_FFI_TYPE_LONG:
        return marshal_int_arg(vm, value, &out->i);
    case CH_FFI_TYPE_BOOL:
        return marshal_bool_arg(vm, value, &out->i);
    case CH_FFI_TYPE_DOUBLE:
    case CH_FFI_TYPE_FLOAT:
        return marshal_double_arg(vm, value, &out->d);
    case CH_FFI_TYPE_POINTER:
        return marshal_pointer_arg(vm, value, &out->p);
    case CH_FFI_TYPE_VOID:
        set_error(vm, "ffi: void is not a valid argument type");
        return -1;
    }
    set_error(vm, "ffi: unknown argument type");
    return -1;
}

static ChValue marshal_return_value(ChVM *vm, ChFFIType type, ChFFIWord word) {
    switch (type) {
    case CH_FFI_TYPE_VOID:
        return CH_VOID;
    case CH_FFI_TYPE_INT:
    case CH_FFI_TYPE_INT8:
    case CH_FFI_TYPE_UINT8:
    case CH_FFI_TYPE_INT16:
    case CH_FFI_TYPE_UINT16:
    case CH_FFI_TYPE_INT32:
    case CH_FFI_TYPE_UINT32:
    case CH_FFI_TYPE_INT64:
    case CH_FFI_TYPE_UINT64:
    case CH_FFI_TYPE_SIZE:
    case CH_FFI_TYPE_LONG:
        return ch_make_integer(&vm->gc, word.i);
    case CH_FFI_TYPE_BOOL:
        return word.i ? CH_TRUE : CH_FALSE;
    case CH_FFI_TYPE_DOUBLE:
    case CH_FFI_TYPE_FLOAT:
        return ch_make_flonum(word.d);
    case CH_FFI_TYPE_POINTER:
        if (!word.p) {
            return CH_FALSE;
        }
        return ch_make_integer(&vm->gc, (int64_t)(uintptr_t)word.p);
    }
    set_error(vm, "ffi: unsupported return type");
    return CH_UNDEFINED;
}

#if defined(_WIN32)
static void *platform_open(const char *path, char *err, size_t err_cap) {
    HMODULE handle = path ? LoadLibraryA(path) : GetModuleHandleA(NULL);
    if (!handle && err && err_cap > 0) {
        snprintf(err, err_cap, "LoadLibrary failed (%lu)", (unsigned long)GetLastError());
    }
    return (void *)handle;
}

static void *platform_lookup(void *handle, const char *symbol, char *err, size_t err_cap) {
    FARPROC proc = GetProcAddress((HMODULE)handle, symbol);
    if (!proc && err && err_cap > 0) {
        snprintf(err, err_cap, "GetProcAddress failed for '%s' (%lu)", symbol,
                 (unsigned long)GetLastError());
    }
    return (void *)proc;
}

static void platform_close(void *handle) {
    if (handle) {
        (void)FreeLibrary((HMODULE)handle);
    }
}
#else
static void *platform_open(const char *path, char *err, size_t err_cap) {
    dlerror();
    void *handle = dlopen(path, RTLD_LAZY | RTLD_LOCAL);
    if (!handle && err && err_cap > 0) {
        const char *msg = dlerror();
        snprintf(err, err_cap, "%s", msg ? msg : "dlopen failed");
    }
    return handle;
}

static void *platform_lookup(void *handle, const char *symbol, char *err, size_t err_cap) {
    dlerror();
    void *sym = dlsym(handle, symbol);
    const char *msg = dlerror();
    if (msg != NULL) {
        if (err && err_cap > 0) {
            snprintf(err, err_cap, "%s", msg);
        }
        return NULL;
    }
    return sym;
}

static void platform_close(void *handle) {
    if (handle) {
        (void)dlclose(handle);
    }
}
#endif

static int try_open_named_library(const char *name, void **out_handle, char *err, size_t err_cap) {
    if (!name || !out_handle) {
        return -1;
    }
    void *handle = platform_open(name, err, err_cap);
    if (handle) {
        *out_handle = handle;
        return 0;
    }

    if (strchr(name, '/') || strchr(name, '\\') || strchr(name, '.')) {
        return -1;
    }

    char candidate[256];
#if defined(_WIN32)
    if (snprintf(candidate, sizeof(candidate), "%s.dll", name) < (int)sizeof(candidate)) {
        handle = platform_open(candidate, err, err_cap);
        if (handle) {
            *out_handle = handle;
            return 0;
        }
    }
#else
    if (snprintf(candidate, sizeof(candidate), "lib%s.dylib", name) < (int)sizeof(candidate)) {
        handle = platform_open(candidate, err, err_cap);
        if (handle) {
            *out_handle = handle;
            return 0;
        }
    }
    if (snprintf(candidate, sizeof(candidate), "lib%s.so", name) < (int)sizeof(candidate)) {
        handle = platform_open(candidate, err, err_cap);
        if (handle) {
            *out_handle = handle;
            return 0;
        }
    }
#endif
    return -1;
}

ChValue ch_gc_make_foreign_library(ChGC *gc, void *handle, int owned) {
    ChForeignLibrary *lib =
        (ChForeignLibrary *)ch_gc_alloc(gc, sizeof(ChForeignLibrary), CH_TAG_FOREIGN_LIBRARY);
    lib->handle = handle;
    lib->owned = owned ? 1 : 0;
    lib->closed = 0;
    return ch_make_pointer(&lib->header);
}

ChValue ch_gc_make_foreign_procedure(ChGC *gc, ChValue library, ChValue name, void *symbol,
                                     ChFFIType result_type, const ChFFIType *arg_types,
                                     uint8_t arity) {
    ch_gc_push(gc, &library);
    ch_gc_push(gc, &name);
    ChForeignProcedure *proc =
        (ChForeignProcedure *)ch_gc_alloc(gc, sizeof(ChForeignProcedure), CH_TAG_FOREIGN_PROC);
    ch_gc_pop_n(gc, 2);
    proc->library = library;
    proc->name = name;
    proc->symbol = symbol;
    proc->arity = arity;
    proc->result_type = (uint8_t)result_type;
    for (uint8_t i = 0; i < CH_FFI_MAX_ARGS; i++) {
        proc->arg_types[i] = (uint8_t)CH_FFI_TYPE_VOID;
    }
    for (uint8_t i = 0; i < arity; i++) {
        proc->arg_types[i] = (uint8_t)arg_types[i];
    }
    return ch_make_pointer(&proc->header);
}

void ch_ffi_finalize_library(ChForeignLibrary *library) {
    if (!library || library->closed) {
        return;
    }
    if (library->owned && library->handle) {
        platform_close(library->handle);
    }
    library->closed = 1;
    library->handle = NULL;
}

int ch_ffi_open_library(ChVM *vm, const char *path, ChValue *out_library) {
    if (!out_library) {
        set_error(vm, "ffi: internal open-library error");
        return -1;
    }

    char err[256] = {0};
    void *handle = NULL;
    int owned = 1;

    if (!path) {
        handle = platform_open(NULL, err, sizeof(err));
        owned = 0; /* process handle */
    } else {
        (void)try_open_named_library(path, &handle, err, sizeof(err));
    }

    if (!handle) {
        snprintf(vm->error, sizeof(vm->error), "open-foreign-library: %s",
                 err[0] ? err : "unable to load shared library");
        return -1;
    }

    *out_library = ch_gc_make_foreign_library(&vm->gc, handle, owned);
    return 0;
}

int ch_ffi_close_library(ChVM *vm, ChValue library) {
    if (!ch_is_foreign_library(library)) {
        set_error(vm, "close-foreign-library!: expected foreign library");
        return -1;
    }
    ChForeignLibrary *lib = ch_as_foreign_library(library);
    ch_ffi_finalize_library(lib);
    return 0;
}

int ch_ffi_lookup_procedure(ChVM *vm, ChValue library, const char *symbol_name,
                            ChFFIType result_type, const ChFFIType *arg_types, uint8_t arity,
                            ChValue *out_proc) {
    if (!out_proc || !symbol_name) {
        set_error(vm, "foreign-procedure: internal lookup error");
        return -1;
    }
    if (!ch_is_foreign_library(library)) {
        set_error(vm, "foreign-procedure: expected foreign library");
        return -1;
    }
    if (arity > CH_FFI_MAX_ARGS) {
        snprintf(vm->error, sizeof(vm->error),
                 "foreign-procedure: supports at most %d arguments", CH_FFI_MAX_ARGS);
        return -1;
    }

    ChForeignLibrary *lib = ch_as_foreign_library(library);
    if (lib->closed || !lib->handle) {
        set_error(vm, "foreign-procedure: library is closed");
        return -1;
    }

    char err[256] = {0};
    void *symbol = platform_lookup(lib->handle, symbol_name, err, sizeof(err));
    if (!symbol) {
        snprintf(vm->error, sizeof(vm->error), "foreign-procedure: %s",
                 err[0] ? err : "symbol not found");
        return -1;
    }

    ChValue name_v = ch_gc_make_string_cstr(&vm->gc, symbol_name);
    *out_proc =
        ch_gc_make_foreign_procedure(&vm->gc, library, name_v, symbol, result_type, arg_types, arity);
    return 0;
}

static int invoke_ffi0(void *symbol, ChFFIType result_type, ChFFIWord *out_word) {
    switch (ffi_call_class(result_type)) {
    case CH_FFI_CLASS_INT: {
        if (result_type == CH_FFI_TYPE_VOID) {
            void (*fn)(void) = NULL;
            memcpy(&fn, &symbol, sizeof(fn));
            fn();
            return 0;
        }
        int64_t (*fn)(void) = NULL;
        memcpy(&fn, &symbol, sizeof(fn));
        return store_int_result(result_type, fn(), out_word);
    }
    case CH_FFI_CLASS_DOUBLE: {
        if (result_type == CH_FFI_TYPE_VOID) {
            void (*fn)(void) = NULL;
            memcpy(&fn, &symbol, sizeof(fn));
            fn();
            return 0;
        }
        double (*fn)(void) = NULL;
        memcpy(&fn, &symbol, sizeof(fn));
        out_word->d = fn();
        return 0;
    }
    case CH_FFI_CLASS_POINTER: {
        if (result_type == CH_FFI_TYPE_VOID) {
            void (*fn)(void) = NULL;
            memcpy(&fn, &symbol, sizeof(fn));
            fn();
            return 0;
        }
        void *(*fn)(void) = NULL;
        memcpy(&fn, &symbol, sizeof(fn));
        out_word->p = fn();
        return 0;
    }
    }
    return -1;
}

static int invoke_ffi1(void *symbol, ChFFIType result_type, ChFFIType arg0_type, ChFFIWord arg0,
                       ChFFIWord *out_word) {
    switch (ffi_call_class(arg0_type)) {
    case CH_FFI_CLASS_INT:
        switch (ffi_call_class(result_type)) {
        case CH_FFI_CLASS_INT: {
            if (result_type == CH_FFI_TYPE_VOID) {
                void (*fn)(int64_t) = NULL;
                memcpy(&fn, &symbol, sizeof(fn));
                fn(arg0.i);
                return 0;
            }
            int64_t (*fn)(int64_t) = NULL;
            memcpy(&fn, &symbol, sizeof(fn));
            return store_int_result(result_type, fn(arg0.i), out_word);
        }
        case CH_FFI_CLASS_DOUBLE: {
            if (result_type == CH_FFI_TYPE_VOID) {
                void (*fn)(int64_t) = NULL;
                memcpy(&fn, &symbol, sizeof(fn));
                fn(arg0.i);
                return 0;
            }
            double (*fn)(int64_t) = NULL;
            memcpy(&fn, &symbol, sizeof(fn));
            out_word->d = fn(arg0.i);
            return 0;
        }
        case CH_FFI_CLASS_POINTER: {
            if (result_type == CH_FFI_TYPE_VOID) {
                void (*fn)(int64_t) = NULL;
                memcpy(&fn, &symbol, sizeof(fn));
                fn(arg0.i);
                return 0;
            }
            void *(*fn)(int64_t) = NULL;
            memcpy(&fn, &symbol, sizeof(fn));
            out_word->p = fn(arg0.i);
            return 0;
        }
        }
        break;
    case CH_FFI_CLASS_DOUBLE:
        switch (ffi_call_class(result_type)) {
        case CH_FFI_CLASS_INT: {
            if (result_type == CH_FFI_TYPE_VOID) {
                void (*fn)(double) = NULL;
                memcpy(&fn, &symbol, sizeof(fn));
                fn(arg0.d);
                return 0;
            }
            int64_t (*fn)(double) = NULL;
            memcpy(&fn, &symbol, sizeof(fn));
            return store_int_result(result_type, fn(arg0.d), out_word);
        }
        case CH_FFI_CLASS_DOUBLE: {
            if (result_type == CH_FFI_TYPE_VOID) {
                void (*fn)(double) = NULL;
                memcpy(&fn, &symbol, sizeof(fn));
                fn(arg0.d);
                return 0;
            }
            double (*fn)(double) = NULL;
            memcpy(&fn, &symbol, sizeof(fn));
            out_word->d = fn(arg0.d);
            return 0;
        }
        case CH_FFI_CLASS_POINTER: {
            if (result_type == CH_FFI_TYPE_VOID) {
                void (*fn)(double) = NULL;
                memcpy(&fn, &symbol, sizeof(fn));
                fn(arg0.d);
                return 0;
            }
            void *(*fn)(double) = NULL;
            memcpy(&fn, &symbol, sizeof(fn));
            out_word->p = fn(arg0.d);
            return 0;
        }
        }
        break;
    case CH_FFI_CLASS_POINTER:
        switch (ffi_call_class(result_type)) {
        case CH_FFI_CLASS_INT: {
            if (result_type == CH_FFI_TYPE_VOID) {
                void (*fn)(void *) = NULL;
                memcpy(&fn, &symbol, sizeof(fn));
                fn(arg0.p);
                return 0;
            }
            int64_t (*fn)(void *) = NULL;
            memcpy(&fn, &symbol, sizeof(fn));
            return store_int_result(result_type, fn(arg0.p), out_word);
        }
        case CH_FFI_CLASS_DOUBLE: {
            if (result_type == CH_FFI_TYPE_VOID) {
                void (*fn)(void *) = NULL;
                memcpy(&fn, &symbol, sizeof(fn));
                fn(arg0.p);
                return 0;
            }
            double (*fn)(void *) = NULL;
            memcpy(&fn, &symbol, sizeof(fn));
            out_word->d = fn(arg0.p);
            return 0;
        }
        case CH_FFI_CLASS_POINTER: {
            if (result_type == CH_FFI_TYPE_VOID) {
                void (*fn)(void *) = NULL;
                memcpy(&fn, &symbol, sizeof(fn));
                fn(arg0.p);
                return 0;
            }
            void *(*fn)(void *) = NULL;
            memcpy(&fn, &symbol, sizeof(fn));
            out_word->p = fn(arg0.p);
            return 0;
        }
        }
        break;
    }
    return -1;
}

static int invoke_ffi2(void *symbol, ChFFIType result_type, ChFFIType arg0_type, ChFFIWord arg0,
                       ChFFIType arg1_type, ChFFIWord arg1, ChFFIWord *out_word) {
#define CH_FFI_CALL2(ARG0_T, ARG0_F, ARG1_T, ARG1_F)                                                   \
    do {                                                                                               \
        switch (ffi_call_class(result_type)) {                                                         \
        case CH_FFI_CLASS_INT: {                                                                       \
            if (result_type == CH_FFI_TYPE_VOID) {                                                     \
                void (*fn)(ARG0_T, ARG1_T) = NULL;                                                     \
                memcpy(&fn, &symbol, sizeof(fn));                                                      \
                fn(arg0.ARG0_F, arg1.ARG1_F);                                                          \
                return 0;                                                                              \
            }                                                                                          \
            int64_t (*fn)(ARG0_T, ARG1_T) = NULL;                                                      \
            memcpy(&fn, &symbol, sizeof(fn));                                                          \
            return store_int_result(result_type, fn(arg0.ARG0_F, arg1.ARG1_F), out_word);              \
        }                                                                                              \
        case CH_FFI_CLASS_DOUBLE: {                                                                    \
            if (result_type == CH_FFI_TYPE_VOID) {                                                     \
                void (*fn)(ARG0_T, ARG1_T) = NULL;                                                     \
                memcpy(&fn, &symbol, sizeof(fn));                                                      \
                fn(arg0.ARG0_F, arg1.ARG1_F);                                                          \
                return 0;                                                                              \
            }                                                                                          \
            double (*fn)(ARG0_T, ARG1_T) = NULL;                                                       \
            memcpy(&fn, &symbol, sizeof(fn));                                                          \
            out_word->d = fn(arg0.ARG0_F, arg1.ARG1_F);                                                \
            return 0;                                                                                  \
        }                                                                                              \
        case CH_FFI_CLASS_POINTER: {                                                                   \
            if (result_type == CH_FFI_TYPE_VOID) {                                                     \
                void (*fn)(ARG0_T, ARG1_T) = NULL;                                                     \
                memcpy(&fn, &symbol, sizeof(fn));                                                      \
                fn(arg0.ARG0_F, arg1.ARG1_F);                                                          \
                return 0;                                                                              \
            }                                                                                          \
            void *(*fn)(ARG0_T, ARG1_T) = NULL;                                                        \
            memcpy(&fn, &symbol, sizeof(fn));                                                          \
            out_word->p = fn(arg0.ARG0_F, arg1.ARG1_F);                                                \
            return 0;                                                                                  \
        }                                                                                              \
        }                                                                                              \
    } while (0)

    ChFFICallClass a0 = ffi_call_class(arg0_type);
    ChFFICallClass a1 = ffi_call_class(arg1_type);
    if (a0 == CH_FFI_CLASS_INT && a1 == CH_FFI_CLASS_INT) {
        CH_FFI_CALL2(int64_t, i, int64_t, i);
    }
    if (a0 == CH_FFI_CLASS_INT && a1 == CH_FFI_CLASS_DOUBLE) {
        CH_FFI_CALL2(int64_t, i, double, d);
    }
    if (a0 == CH_FFI_CLASS_INT && a1 == CH_FFI_CLASS_POINTER) {
        CH_FFI_CALL2(int64_t, i, void *, p);
    }
    if (a0 == CH_FFI_CLASS_DOUBLE && a1 == CH_FFI_CLASS_INT) {
        CH_FFI_CALL2(double, d, int64_t, i);
    }
    if (a0 == CH_FFI_CLASS_DOUBLE && a1 == CH_FFI_CLASS_DOUBLE) {
        CH_FFI_CALL2(double, d, double, d);
    }
    if (a0 == CH_FFI_CLASS_DOUBLE && a1 == CH_FFI_CLASS_POINTER) {
        CH_FFI_CALL2(double, d, void *, p);
    }
    if (a0 == CH_FFI_CLASS_POINTER && a1 == CH_FFI_CLASS_INT) {
        CH_FFI_CALL2(void *, p, int64_t, i);
    }
    if (a0 == CH_FFI_CLASS_POINTER && a1 == CH_FFI_CLASS_DOUBLE) {
        CH_FFI_CALL2(void *, p, double, d);
    }
    if (a0 == CH_FFI_CLASS_POINTER && a1 == CH_FFI_CLASS_POINTER) {
        CH_FFI_CALL2(void *, p, void *, p);
    }

#undef CH_FFI_CALL2
    return -1;
}

static int invoke_int3(void *symbol, ChFFIType result_type, const int64_t *args, ChFFIWord *out_word) {
    int64_t (*fn)(int64_t, int64_t, int64_t) = NULL;
    memcpy(&fn, &symbol, sizeof(fn));
    if (result_type == CH_FFI_TYPE_VOID) {
        fn(args[0], args[1], args[2]);
        return 0;
    }
    return store_int_result(result_type, fn(args[0], args[1], args[2]), out_word);
}

static int invoke_int4(void *symbol, ChFFIType result_type, const int64_t *args, ChFFIWord *out_word) {
    int64_t (*fn)(int64_t, int64_t, int64_t, int64_t) = NULL;
    memcpy(&fn, &symbol, sizeof(fn));
    if (result_type == CH_FFI_TYPE_VOID) {
        fn(args[0], args[1], args[2], args[3]);
        return 0;
    }
    return store_int_result(result_type, fn(args[0], args[1], args[2], args[3]), out_word);
}

static int invoke_int5(void *symbol, ChFFIType result_type, const int64_t *args, ChFFIWord *out_word) {
    int64_t (*fn)(int64_t, int64_t, int64_t, int64_t, int64_t) = NULL;
    memcpy(&fn, &symbol, sizeof(fn));
    if (result_type == CH_FFI_TYPE_VOID) {
        fn(args[0], args[1], args[2], args[3], args[4]);
        return 0;
    }
    return store_int_result(result_type, fn(args[0], args[1], args[2], args[3], args[4]), out_word);
}

static int invoke_int6(void *symbol, ChFFIType result_type, const int64_t *args, ChFFIWord *out_word) {
    int64_t (*fn)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t) = NULL;
    memcpy(&fn, &symbol, sizeof(fn));
    if (result_type == CH_FFI_TYPE_VOID) {
        fn(args[0], args[1], args[2], args[3], args[4], args[5]);
        return 0;
    }
    return store_int_result(result_type, fn(args[0], args[1], args[2], args[3], args[4], args[5]),
                           out_word);
}

static int invoke_int7(void *symbol, ChFFIType result_type, const int64_t *args, ChFFIWord *out_word) {
    int64_t (*fn)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t) = NULL;
    memcpy(&fn, &symbol, sizeof(fn));
    if (result_type == CH_FFI_TYPE_VOID) {
        fn(args[0], args[1], args[2], args[3], args[4], args[5], args[6]);
        return 0;
    }
    return store_int_result(result_type,
                           fn(args[0], args[1], args[2], args[3], args[4], args[5], args[6]),
                           out_word);
}

static int invoke_int8(void *symbol, ChFFIType result_type, const int64_t *args, ChFFIWord *out_word) {
    int64_t (*fn)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t) = NULL;
    memcpy(&fn, &symbol, sizeof(fn));
    if (result_type == CH_FFI_TYPE_VOID) {
        fn(args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7]);
        return 0;
    }
    return store_int_result(result_type,
                           fn(args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7]),
                           out_word);
}

static int invoke_double_n(void *symbol, int nargs, const double *args, ChFFIType result_type,
                           ChFFIWord *out_word) {
    switch (nargs) {
    case 3: {
        double (*fn)(double, double, double) = NULL;
        memcpy(&fn, &symbol, sizeof(fn));
        out_word->d = fn(args[0], args[1], args[2]);
        return result_type == CH_FFI_TYPE_VOID ? 0 : 0;
    }
    case 4: {
        double (*fn)(double, double, double, double) = NULL;
        memcpy(&fn, &symbol, sizeof(fn));
        out_word->d = fn(args[0], args[1], args[2], args[3]);
        return 0;
    }
    case 5: {
        double (*fn)(double, double, double, double, double) = NULL;
        memcpy(&fn, &symbol, sizeof(fn));
        out_word->d = fn(args[0], args[1], args[2], args[3], args[4]);
        return 0;
    }
    case 6: {
        double (*fn)(double, double, double, double, double, double) = NULL;
        memcpy(&fn, &symbol, sizeof(fn));
        out_word->d = fn(args[0], args[1], args[2], args[3], args[4], args[5]);
        return 0;
    }
    case 7: {
        double (*fn)(double, double, double, double, double, double, double) = NULL;
        memcpy(&fn, &symbol, sizeof(fn));
        out_word->d = fn(args[0], args[1], args[2], args[3], args[4], args[5], args[6]);
        return 0;
    }
    case 8: {
        double (*fn)(double, double, double, double, double, double, double, double) = NULL;
        memcpy(&fn, &symbol, sizeof(fn));
        out_word->d = fn(args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7]);
        return 0;
    }
    default:
        return -1;
    }
}

static int invoke_pointer_n(void *symbol, int nargs, void *const *args, ChFFIType result_type,
                            ChFFIWord *out_word) {
    switch (nargs) {
    case 3: {
        void *(*fn)(void *, void *, void *) = NULL;
        memcpy(&fn, &symbol, sizeof(fn));
        out_word->p = fn(args[0], args[1], args[2]);
        return result_type == CH_FFI_TYPE_VOID ? 0 : 0;
    }
    case 4: {
        void *(*fn)(void *, void *, void *, void *) = NULL;
        memcpy(&fn, &symbol, sizeof(fn));
        out_word->p = fn(args[0], args[1], args[2], args[3]);
        return 0;
    }
    case 5: {
        void *(*fn)(void *, void *, void *, void *, void *) = NULL;
        memcpy(&fn, &symbol, sizeof(fn));
        out_word->p = fn(args[0], args[1], args[2], args[3], args[4]);
        return 0;
    }
    case 6: {
        void *(*fn)(void *, void *, void *, void *, void *, void *) = NULL;
        memcpy(&fn, &symbol, sizeof(fn));
        out_word->p = fn(args[0], args[1], args[2], args[3], args[4], args[5]);
        return 0;
    }
    case 7: {
        void *(*fn)(void *, void *, void *, void *, void *, void *, void *) = NULL;
        memcpy(&fn, &symbol, sizeof(fn));
        out_word->p = fn(args[0], args[1], args[2], args[3], args[4], args[5], args[6]);
        return 0;
    }
    case 8: {
        void *(*fn)(void *, void *, void *, void *, void *, void *, void *, void *) = NULL;
        memcpy(&fn, &symbol, sizeof(fn));
        out_word->p = fn(args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7]);
        return 0;
    }
    default:
        return -1;
    }
}

static int invoke_ffi_many(void *symbol, ChFFIType result_type, const ChFFIType *arg_types,
                           const ChFFIWord *marshaled, int nargs, ChFFIWord *out_word) {
    if (nargs < 3 || nargs > CH_FFI_MAX_ARGS) {
        return -1;
    }
    ChFFICallClass cls = ffi_call_class(arg_types[0]);
    for (int i = 1; i < nargs; i++) {
        ChFFICallClass other = ffi_call_class(arg_types[i]);
        if (other != cls && !(cls == CH_FFI_CLASS_INT && other == CH_FFI_CLASS_POINTER) &&
            !(cls == CH_FFI_CLASS_POINTER && other == CH_FFI_CLASS_INT)) {
            return -1;
        }
    }

    if (cls == CH_FFI_CLASS_DOUBLE) {
        double dargs[CH_FFI_MAX_ARGS];
        for (int i = 0; i < nargs; i++) {
            dargs[i] = marshaled[i].d;
        }
        return invoke_double_n(symbol, nargs, dargs, result_type, out_word);
    }

    if (cls == CH_FFI_CLASS_POINTER ||
        (cls == CH_FFI_CLASS_INT && ffi_call_class(arg_types[0]) == CH_FFI_CLASS_POINTER)) {
        void *pargs[CH_FFI_MAX_ARGS];
        for (int i = 0; i < nargs; i++) {
            pargs[i] = marshaled[i].p;
        }
        return invoke_pointer_n(symbol, nargs, pargs, result_type, out_word);
    }

    int64_t iargs[CH_FFI_MAX_ARGS];
    for (int i = 0; i < nargs; i++) {
        if (ffi_call_class(arg_types[i]) == CH_FFI_CLASS_POINTER) {
            iargs[i] = (int64_t)(uintptr_t)marshaled[i].p;
        } else {
            iargs[i] = marshaled[i].i;
        }
    }
    switch (nargs) {
    case 3:
        return invoke_int3(symbol, result_type, iargs, out_word);
    case 4:
        return invoke_int4(symbol, result_type, iargs, out_word);
    case 5:
        return invoke_int5(symbol, result_type, iargs, out_word);
    case 6:
        return invoke_int6(symbol, result_type, iargs, out_word);
    case 7:
        return invoke_int7(symbol, result_type, iargs, out_word);
    case 8:
        return invoke_int8(symbol, result_type, iargs, out_word);
    default:
        return -1;
    }
}

int ch_ffi_call(ChVM *vm, ChForeignProcedure *proc, ChValue *args, int nargs, ChValue *out) {
    if (!proc || !out) {
        set_error(vm, "ffi: internal call error");
        return -1;
    }
    if (proc->arity != (uint8_t)nargs) {
        snprintf(vm->error, sizeof(vm->error),
                 "foreign-procedure: expected %u args, got %d", (unsigned)proc->arity, nargs);
        return -1;
    }
    if (!ch_is_foreign_library(proc->library)) {
        set_error(vm, "foreign-procedure: library handle is invalid");
        return -1;
    }
    ChForeignLibrary *lib = ch_as_foreign_library(proc->library);
    if (lib->closed || !lib->handle) {
        set_error(vm, "foreign-procedure: library is closed");
        return -1;
    }

    ChFFIWord marshaled[CH_FFI_MAX_ARGS];
    memset(marshaled, 0, sizeof(marshaled));
    for (int i = 0; i < nargs; i++) {
        if (marshal_arg(vm, (ChFFIType)proc->arg_types[i], args[i], &marshaled[i]) != 0) {
            return -1;
        }
    }

    ChFFIWord raw_result;
    memset(&raw_result, 0, sizeof(raw_result));

    int rc = -1;
    if (nargs == 0) {
        rc = invoke_ffi0(proc->symbol, (ChFFIType)proc->result_type, &raw_result);
    } else if (nargs == 1) {
        rc = invoke_ffi1(proc->symbol, (ChFFIType)proc->result_type, (ChFFIType)proc->arg_types[0],
                         marshaled[0], &raw_result);
    } else if (nargs == 2) {
        rc = invoke_ffi2(proc->symbol, (ChFFIType)proc->result_type, (ChFFIType)proc->arg_types[0],
                         marshaled[0], (ChFFIType)proc->arg_types[1], marshaled[1], &raw_result);
    } else {
        rc = invoke_ffi_many(proc->symbol, (ChFFIType)proc->result_type,
                             (const ChFFIType *)(const void *)proc->arg_types, marshaled, nargs,
                             &raw_result);
    }
    if (rc != 0) {
        set_error(vm, "foreign-procedure: unsupported signature");
        return -1;
    }

    *out = marshal_return_value(vm, (ChFFIType)proc->result_type, raw_result);
    return *out == CH_UNDEFINED ? -1 : 0;
}
