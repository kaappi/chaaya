#include "chaaya/ffi_callback.h"

#include "chaaya/bignum.h"
#include "chaaya/vm.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct ChFFICallbackSlot {
    ChVM *vm;
    ChValue proc;
    ChFFICallbackSig sig;
    uint8_t active;
} ChFFICallbackSlot;

static ChFFICallbackSlot g_slots[CH_FFI_CALLBACK_SLOTS];

static void ch_ffi_cb_invoke_v_void(int idx);
static void ch_ffi_cb_invoke_p_void(int idx, void *a);
static int ch_ffi_cb_invoke_p_int(int idx, void *a);
static int ch_ffi_cb_invoke_pp_int(int idx, void *a, void *b);

#define CH_FFI_CB_DEFINE_V_VOID(N)                                                                         \
    static void ch_ffi_cb_trampoline_v_void_##N(void) { ch_ffi_cb_invoke_v_void(N); }

#define CH_FFI_CB_DEFINE_P_VOID(N)                                                                         \
    static void ch_ffi_cb_trampoline_p_void_##N(void *a) { ch_ffi_cb_invoke_p_void(N, a); }

#define CH_FFI_CB_DEFINE_P_INT(N)                                                                        \
    static int ch_ffi_cb_trampoline_p_int_##N(void *a) { return ch_ffi_cb_invoke_p_int(N, a); }

#define CH_FFI_CB_DEFINE_PP_INT(N)                                                                         \
    static int ch_ffi_cb_trampoline_pp_int_##N(void *a, void *b) {                                       \
        return ch_ffi_cb_invoke_pp_int(N, a, b);                                                         \
    }

#define CH_FFI_CB_DEFINE_ALL(N)                                                                            \
    CH_FFI_CB_DEFINE_V_VOID(N)                                                                             \
    CH_FFI_CB_DEFINE_P_VOID(N)                                                                             \
    CH_FFI_CB_DEFINE_P_INT(N)                                                                              \
    CH_FFI_CB_DEFINE_PP_INT(N)

CH_FFI_CB_DEFINE_ALL(0)
CH_FFI_CB_DEFINE_ALL(1)
CH_FFI_CB_DEFINE_ALL(2)
CH_FFI_CB_DEFINE_ALL(3)
CH_FFI_CB_DEFINE_ALL(4)
CH_FFI_CB_DEFINE_ALL(5)
CH_FFI_CB_DEFINE_ALL(6)
CH_FFI_CB_DEFINE_ALL(7)
CH_FFI_CB_DEFINE_ALL(8)
CH_FFI_CB_DEFINE_ALL(9)
CH_FFI_CB_DEFINE_ALL(10)
CH_FFI_CB_DEFINE_ALL(11)
CH_FFI_CB_DEFINE_ALL(12)
CH_FFI_CB_DEFINE_ALL(13)
CH_FFI_CB_DEFINE_ALL(14)
CH_FFI_CB_DEFINE_ALL(15)

#undef CH_FFI_CB_DEFINE_ALL
#undef CH_FFI_CB_DEFINE_PP_INT
#undef CH_FFI_CB_DEFINE_P_INT
#undef CH_FFI_CB_DEFINE_P_VOID
#undef CH_FFI_CB_DEFINE_V_VOID

static void *g_trampolines_v_void[CH_FFI_CALLBACK_SLOTS];
static void *g_trampolines_p_void[CH_FFI_CALLBACK_SLOTS];
static void *g_trampolines_p_int[CH_FFI_CALLBACK_SLOTS];
static void *g_trampolines_pp_int[CH_FFI_CALLBACK_SLOTS];

static int g_trampolines_initialized;

static void init_trampolines(void) {
    if (g_trampolines_initialized) {
        return;
    }
#define CH_FFI_CB_INIT(N)                                                                                  \
    g_trampolines_v_void[N] = (void *)(uintptr_t)ch_ffi_cb_trampoline_v_void_##N;                        \
    g_trampolines_p_void[N] = (void *)(uintptr_t)ch_ffi_cb_trampoline_p_void_##N;                          \
    g_trampolines_p_int[N] = (void *)(uintptr_t)ch_ffi_cb_trampoline_p_int_##N;                            \
    g_trampolines_pp_int[N] = (void *)(uintptr_t)ch_ffi_cb_trampoline_pp_int_##N
    CH_FFI_CB_INIT(0);
    CH_FFI_CB_INIT(1);
    CH_FFI_CB_INIT(2);
    CH_FFI_CB_INIT(3);
    CH_FFI_CB_INIT(4);
    CH_FFI_CB_INIT(5);
    CH_FFI_CB_INIT(6);
    CH_FFI_CB_INIT(7);
    CH_FFI_CB_INIT(8);
    CH_FFI_CB_INIT(9);
    CH_FFI_CB_INIT(10);
    CH_FFI_CB_INIT(11);
    CH_FFI_CB_INIT(12);
    CH_FFI_CB_INIT(13);
    CH_FFI_CB_INIT(14);
    CH_FFI_CB_INIT(15);
#undef CH_FFI_CB_INIT
    g_trampolines_initialized = 1;
}

static void *trampoline_for_slot(ChFFICallbackSig sig, int idx) {
    switch (sig) {
    case CH_FFI_CB_V_VOID:
        return g_trampolines_v_void[idx];
    case CH_FFI_CB_P_VOID:
        return g_trampolines_p_void[idx];
    case CH_FFI_CB_P_INT:
        return g_trampolines_p_int[idx];
    case CH_FFI_CB_PP_INT:
        return g_trampolines_pp_int[idx];
    }
    return NULL;
}

static int slot_for_trampoline(void *fn_ptr, int *out_idx, ChFFICallbackSig *out_sig) {
    if (!fn_ptr) {
        return 0;
    }
    for (int i = 0; i < CH_FFI_CALLBACK_SLOTS; i++) {
        if (fn_ptr == g_trampolines_v_void[i]) {
            *out_idx = i;
            *out_sig = CH_FFI_CB_V_VOID;
            return 1;
        }
        if (fn_ptr == g_trampolines_p_void[i]) {
            *out_idx = i;
            *out_sig = CH_FFI_CB_P_VOID;
            return 1;
        }
        if (fn_ptr == g_trampolines_p_int[i]) {
            *out_idx = i;
            *out_sig = CH_FFI_CB_P_INT;
            return 1;
        }
        if (fn_ptr == g_trampolines_pp_int[i]) {
            *out_idx = i;
            *out_sig = CH_FFI_CB_PP_INT;
            return 1;
        }
    }
    return 0;
}

static ChValue marshal_ptr_arg(ChVM *vm, void *ptr) {
    if (!ptr) {
        return ch_make_integer(&vm->gc, 0);
    }
    uintptr_t addr = (uintptr_t)ptr;
    if (addr <= (uintptr_t)INT64_MAX) {
        return ch_make_integer(&vm->gc, (int64_t)addr);
    }
    uint64_t limbs[1] = {(uint64_t)addr};
    return ch_gc_make_bignum_from_limbs(&vm->gc, limbs, 1, 1);
}

static void stash_vm_error(ChVM *vm) {
    if (vm->ffi_callback_deferred) {
        vm->error[0] = '\0';
        return;
    }
    vm->ffi_callback_deferred = true;
    if (vm->ffi_callback_deferred_value != CH_UNDEFINED) {
        vm->error[0] = '\0';
        return;
    }
    const char *detail = vm->error[0] ? vm->error : "error in FFI callback";
    ChValue msg = ch_gc_make_string_cstr(&vm->gc, detail);
    ch_gc_push(&vm->gc, &msg);
    vm->ffi_callback_deferred_value = ch_gc_make_error_object(&vm->gc, msg, CH_NIL, 0);
    ch_gc_pop(&vm->gc);
    vm->error[0] = '\0';
}

static void note_callback_error(ChVM *vm, ChValue payload) {
    if (vm->ffi_callback_deferred) {
        return;
    }
    vm->ffi_callback_deferred = true;
    if (payload != CH_UNDEFINED) {
        vm->ffi_callback_deferred_value = payload;
        return;
    }
    stash_vm_error(vm);
}

static int invoke_callback(ChFFICallbackSlot *slot, ChValue *args, int nargs, ChValue *out) {
    ChVM *vm = slot->vm;
    vm->ffi_callback_depth++;
    vm->error[0] = '\0';
    ChVMStatus st = ch_vm_apply(vm, slot->proc, args, nargs, out);
    vm->ffi_callback_depth--;

    if (vm->ffi_callback_deferred) {
        return -1;
    }
    if (st == CH_VM_CONTINUATION_INVOKED || st == CH_VM_FIBER_PARKED) {
        note_callback_error(vm, CH_UNDEFINED);
        return -1;
    }
    if (st != CH_VM_OK || vm->error[0]) {
        note_callback_error(vm, CH_UNDEFINED);
        return -1;
    }
    return 0;
}

static int marshal_int_return(ChVM *vm, ChValue result) {
    if (ch_is_fixnum(result)) {
        int64_t v = ch_to_fixnum(result);
        if (v >= INT_MIN && v <= INT_MAX) {
            return (int)v;
        }
    }
    note_callback_error(vm, CH_UNDEFINED);
    if (vm->ffi_callback_deferred_value == CH_UNDEFINED) {
        ChValue msg = ch_gc_make_string_cstr(&vm->gc, "FFI callback must return a C int");
        ch_gc_push(&vm->gc, &msg);
        vm->ffi_callback_deferred_value = ch_gc_make_error_object(&vm->gc, msg, CH_NIL, 0);
        ch_gc_pop(&vm->gc);
    }
    return 0;
}

static void ch_ffi_cb_invoke_v_void(int idx) {
    if (idx < 0 || idx >= CH_FFI_CALLBACK_SLOTS) {
        return;
    }
    ChFFICallbackSlot *slot = &g_slots[idx];
    if (!slot->active || !slot->vm) {
        return;
    }
    ChValue ignored = CH_VOID;
    (void)invoke_callback(slot, NULL, 0, &ignored);
}

static void ch_ffi_cb_invoke_p_void(int idx, void *a) {
    if (idx < 0 || idx >= CH_FFI_CALLBACK_SLOTS) {
        return;
    }
    ChFFICallbackSlot *slot = &g_slots[idx];
    if (!slot->active || !slot->vm) {
        return;
    }
    ChVM *vm = slot->vm;
    ChValue arg0 = marshal_ptr_arg(vm, a);
    ChValue ignored = CH_VOID;
    (void)invoke_callback(slot, &arg0, 1, &ignored);
}

static int ch_ffi_cb_invoke_p_int(int idx, void *a) {
    if (idx < 0 || idx >= CH_FFI_CALLBACK_SLOTS) {
        return 0;
    }
    ChFFICallbackSlot *slot = &g_slots[idx];
    if (!slot->active || !slot->vm) {
        return 0;
    }
    ChVM *vm = slot->vm;
    ChValue arg0 = marshal_ptr_arg(vm, a);
    ChValue result = CH_VOID;
    if (invoke_callback(slot, &arg0, 1, &result) != 0) {
        return 0;
    }
    return marshal_int_return(vm, result);
}

static int ch_ffi_cb_invoke_pp_int(int idx, void *a, void *b) {
    if (idx < 0 || idx >= CH_FFI_CALLBACK_SLOTS) {
        return 0;
    }
    ChFFICallbackSlot *slot = &g_slots[idx];
    if (!slot->active || !slot->vm) {
        return 0;
    }
    ChVM *vm = slot->vm;
    ChValue args[2] = {marshal_ptr_arg(vm, a), marshal_ptr_arg(vm, b)};
    ChValue result = CH_VOID;
    if (invoke_callback(slot, args, 2, &result) != 0) {
        return 0;
    }
    return marshal_int_return(vm, result);
}

int ch_ffi_callback_parse_sig(const char *sig, ChFFICallbackSig *out) {
    if (!sig || !out) {
        return 0;
    }
    if (strcmp(sig, "v_void") == 0) {
        *out = CH_FFI_CB_V_VOID;
        return 1;
    }
    if (strcmp(sig, "p_void") == 0) {
        *out = CH_FFI_CB_P_VOID;
        return 1;
    }
    if (strcmp(sig, "p_int") == 0) {
        *out = CH_FFI_CB_P_INT;
        return 1;
    }
    if (strcmp(sig, "pp_int") == 0) {
        *out = CH_FFI_CB_PP_INT;
        return 1;
    }
    return 0;
}

int ch_ffi_callback_match_sig(const ChFFIType *param_types, uint8_t param_count, ChFFIType result_type,
                              ChFFICallbackSig *out) {
    if (!out) {
        return 0;
    }
    if (param_count == 0 && result_type == CH_FFI_TYPE_VOID) {
        *out = CH_FFI_CB_V_VOID;
        return 1;
    }
    if (param_count == 1 && param_types[0] == CH_FFI_TYPE_POINTER && result_type == CH_FFI_TYPE_VOID) {
        *out = CH_FFI_CB_P_VOID;
        return 1;
    }
    if (param_count == 1 && param_types[0] == CH_FFI_TYPE_POINTER && result_type == CH_FFI_TYPE_INT) {
        *out = CH_FFI_CB_P_INT;
        return 1;
    }
    if (param_count == 2 && param_types[0] == CH_FFI_TYPE_POINTER &&
        param_types[1] == CH_FFI_TYPE_POINTER && result_type == CH_FFI_TYPE_INT) {
        *out = CH_FFI_CB_PP_INT;
        return 1;
    }
    return 0;
}

void *ch_ffi_callback_make_sig(ChVM *vm, ChValue proc, ChFFICallbackSig cb_sig, char *err,
                               size_t errlen) {
    init_trampolines();
    if (!vm || !ch_is_procedure(proc)) {
        if (err && errlen > 0) {
            snprintf(err, errlen, "ffi-callback: expected procedure");
        }
        return NULL;
    }
    for (int i = 0; i < CH_FFI_CALLBACK_SLOTS; i++) {
        if (g_slots[i].active) {
            continue;
        }
        g_slots[i].vm = vm;
        g_slots[i].proc = proc;
        g_slots[i].sig = cb_sig;
        g_slots[i].active = 1;
        return trampoline_for_slot(cb_sig, i);
    }
    if (err && errlen > 0) {
        snprintf(err, errlen, "ffi-callback: no free callback slots (max %d)", CH_FFI_CALLBACK_SLOTS);
    }
    return NULL;
}

void *ch_ffi_callback_make(ChVM *vm, ChValue proc, const char *sig, char *err, size_t errlen) {
    ChFFICallbackSig cb_sig;
    if (!ch_ffi_callback_parse_sig(sig, &cb_sig)) {
        if (err && errlen > 0) {
            snprintf(err, errlen, "ffi-callback: unsupported callback signature");
        }
        return NULL;
    }
    return ch_ffi_callback_make_sig(vm, proc, cb_sig, err, errlen);
}

void ch_ffi_callback_release(void *fn_ptr) {
    init_trampolines();
    int idx = 0;
    ChFFICallbackSig sig;
    if (!slot_for_trampoline(fn_ptr, &idx, &sig)) {
        return;
    }
    (void)sig;
    g_slots[idx].active = 0;
    g_slots[idx].vm = NULL;
    g_slots[idx].proc = CH_UNDEFINED;
}

int ch_ffi_callback_p(void *fn_ptr) {
    init_trampolines();
    int idx = 0;
    ChFFICallbackSig sig;
    return slot_for_trampoline(fn_ptr, &idx, &sig);
}

void ch_ffi_callback_mark_roots(void) {
    for (int i = 0; i < CH_FFI_CALLBACK_SLOTS; i++) {
        if (g_slots[i].active) {
            ch_gc_mark_value(g_slots[i].proc);
        }
    }
}

int ch_ffi_callback_raise_deferred(ChVM *vm) {
    if (!vm || !vm->ffi_callback_deferred) {
        return 0;
    }
    vm->ffi_callback_deferred = false;
    ChValue payload = vm->ffi_callback_deferred_value;
    vm->ffi_callback_deferred_value = CH_UNDEFINED;

    if (payload == CH_UNDEFINED) {
        snprintf(vm->error, sizeof(vm->error), "error in FFI callback");
        return -1;
    }

    vm->error[0] = '\0';
    vm->ffi_callback_raise_result = ch_vm_raise(vm, payload, 0);
    if (vm->continuation_invoked) {
        return -1;
    }
    if (vm->error[0]) {
        return -1;
    }
    return 1;
}
