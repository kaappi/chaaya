#ifndef CHAAYA_FFI_H
#define CHAAYA_FFI_H

#include "chaaya/gc.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ChVM ChVM;

typedef enum ChFFIType {
    CH_FFI_TYPE_VOID = 0,
    CH_FFI_TYPE_INT = 1,
    CH_FFI_TYPE_DOUBLE = 2,
    CH_FFI_TYPE_POINTER = 3,
    CH_FFI_TYPE_FLOAT = 4,
    CH_FFI_TYPE_BOOL = 5,
    CH_FFI_TYPE_INT8 = 6,
    CH_FFI_TYPE_UINT8 = 7,
    CH_FFI_TYPE_INT16 = 8,
    CH_FFI_TYPE_UINT16 = 9,
    CH_FFI_TYPE_INT32 = 10,
    CH_FFI_TYPE_UINT32 = 11,
    CH_FFI_TYPE_INT64 = 12,
    CH_FFI_TYPE_UINT64 = 13,
    CH_FFI_TYPE_SIZE = 14,
    CH_FFI_TYPE_LONG = 15,
} ChFFIType;

#define CH_FFI_MAX_ARGS 8

typedef struct ChForeignLibrary {
    ChObject header;
    void *handle;
    uint8_t owned;
    uint8_t closed;
} ChForeignLibrary;

typedef struct ChForeignProcedure {
    ChObject header;
    ChValue library;
    ChValue name;
    void *symbol;
    uint8_t arity;
    uint8_t result_type;
    uint8_t arg_types[CH_FFI_MAX_ARGS];
} ChForeignProcedure;

ChValue ch_gc_make_foreign_library(ChGC *gc, void *handle, int owned);
ChValue ch_gc_make_foreign_procedure(ChGC *gc, ChValue library, ChValue name, void *symbol,
                                     ChFFIType result_type, const ChFFIType *arg_types,
                                     uint8_t arity);

int ch_ffi_open_library(ChVM *vm, const char *path, ChValue *out_library);
int ch_ffi_close_library(ChVM *vm, ChValue library);
int ch_ffi_lookup_procedure(ChVM *vm, ChValue library, const char *symbol_name,
                            ChFFIType result_type, const ChFFIType *arg_types, uint8_t arity,
                            ChValue *out_proc);
int ch_ffi_call(ChVM *vm, ChForeignProcedure *proc, ChValue *args, int nargs, ChValue *out);

int ch_ffi_parse_type_symbol(ChValue sym, ChFFIType *out_type);
const char *ch_ffi_type_name(ChFFIType type);
void ch_ffi_finalize_library(ChForeignLibrary *library);

/* Thin wrapper around ch_sandbox_deny_ffi(): sets vm->error to "<who>: denied
 * by sandbox" and returns -1 when the sandbox forbids FFI; returns 0
 * otherwise. Call from every FFI entry point (open/lookup/callback). */
int ch_ffi_check_sandbox(ChVM *vm, const char *who);

#ifdef __cplusplus
}
#endif

#endif /* CHAAYA_FFI_H */
