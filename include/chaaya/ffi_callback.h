#ifndef CHAAYA_FFI_CALLBACK_H
#define CHAAYA_FFI_CALLBACK_H

#include "chaaya/ffi.h"
#include "chaaya/value.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ChVM ChVM;

typedef enum ChFFICallbackSig {
    CH_FFI_CB_V_VOID = 0,  /* void (*)(void) */
    CH_FFI_CB_P_VOID = 1,  /* void (*)(void *) */
    CH_FFI_CB_P_INT = 2,   /* int (*)(void *) */
    CH_FFI_CB_PP_INT = 3,  /* int (*)(void *, void *) */
} ChFFICallbackSig;

#define CH_FFI_CALLBACK_SLOTS 16

void *ch_ffi_callback_make(ChVM *vm, ChValue proc, const char *sig, char *err, size_t errlen);
void *ch_ffi_callback_make_sig(ChVM *vm, ChValue proc, ChFFICallbackSig sig, char *err,
                               size_t errlen);
void ch_ffi_callback_release(void *fn_ptr);
int ch_ffi_callback_p(void *fn_ptr);

/* Mark active callback procedures during GC. */
void ch_ffi_callback_mark_roots(void);

/* Re-raise a deferred callback error after the enclosing foreign call returns.
 * Returns 0 if none, 1 if a deferred exception was delivered (see
 * vm->ffi_callback_raise_result), -1 on failure (vm->error set). */
int ch_ffi_callback_raise_deferred(ChVM *vm);

/* Parse signature kind names: v_void, p_void, p_int, pp_int. */
int ch_ffi_callback_parse_sig(const char *sig, ChFFICallbackSig *out);

/* Map Kaappi-style FFI type lists to a supported callback signature, or -1. */
int ch_ffi_callback_match_sig(const ChFFIType *param_types, uint8_t param_count,
                              ChFFIType result_type, ChFFICallbackSig *out);

#ifdef __cplusplus
}
#endif

#endif /* CHAAYA_FFI_CALLBACK_H */
