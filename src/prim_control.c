#include "chaaya/prim.h"

#include "chaaya/eval.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void define_prim(ChVM *vm, const char *name, ChNativeFn fn, int arity, int min_arity) {
    ChValue sym = ch_gc_intern_symbol_cstr(&vm->gc, name);
    ChSymbol *s = ch_as_symbol(sym);
    int idx = ch_vm_intern_global(vm, s);
    ChValue nv = ch_gc_make_native(&vm->gc, fn, name, arity, min_arity);
    ch_vm_define_global(vm, idx, nv);
}

/* Map apply status into a native return value; propagate continuation restores. */
static ChValue finish_apply(ChVM *vm, ChVMStatus st, ChValue result) {
    if (st == CH_VM_CONTINUATION_INVOKED) {
        vm->continuation_invoked = true;
        return CH_UNDEFINED;
    }
    if (st != CH_VM_OK) {
        return CH_UNDEFINED;
    }
    return result;
}

static ChValue prim_call_cc(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    ChValue receiver = args[0];
    if (!ch_is_procedure(receiver)) {
        snprintf(vm->error, sizeof(vm->error), "call/cc: receiver not a procedure");
        return CH_UNDEFINED;
    }
    bool was_tail = vm->native_was_tail;
    ChValue cont = ch_vm_capture_continuation(vm, vm->native_result_slot);
    if (was_tail) {
        vm->has_pending_call = true;
        vm->pending_call_tail = true;
        vm->pending_proc = receiver;
        vm->pending_nargs = 1;
        vm->pending_args[0] = cont;
        return CH_UNDEFINED;
    }
    ChValue call_args[1] = {cont};
    ChValue result = CH_VOID;
    return finish_apply(vm, ch_vm_apply(vm, receiver, call_args, 1, &result), result);
}

/* Escape-only continuation: valid only within call/ec's dynamic extent.
 * Always nests apply (no pending TCO) so we can clear `valid` when the
 * extent ends — whether the receiver returned or escaped through us. */
static ChValue prim_call_ec(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    ChValue receiver = args[0];
    if (!ch_is_procedure(receiver)) {
        snprintf(vm->error, sizeof(vm->error), "call/ec: receiver not a procedure");
        return CH_UNDEFINED;
    }
    ChValue cont = ch_vm_capture_escape(vm, vm->native_result_slot);
    ch_gc_push(&vm->gc, &cont);
    ChContinuation *c = ch_as_continuation(cont);
    ChValue call_args[1] = {cont};
    ChValue result = CH_VOID;
    ChVMStatus st = ch_vm_apply(vm, receiver, call_args, 1, &result);
    /* Fiber park unwinds this native while the Scheme extent is still live
     * (snapshot + resume re-enters the parked primitive). Keep `valid` so a
     * later guard escape after resume still works; invoke_escape clears it. */
    if (st != CH_VM_FIBER_PARKED) {
        c->valid = false;
    }
    ch_gc_pop(&vm->gc);
    return finish_apply(vm, st, result);
}

static ChValue prim_push_wind(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    if (ch_vm_ensure_wind_capacity(vm, vm->wind_count + 1) != 0) {
        return CH_UNDEFINED;
    }
    vm->wind_stack[vm->wind_count].before = args[0];
    vm->wind_stack[vm->wind_count].after = args[1];
    vm->wind_count++;
    return CH_VOID;
}

static ChValue prim_pop_wind(ChVM *vm, ChValue *args, int nargs) {
    (void)args;
    (void)nargs;
    if (vm->wind_count == 0) {
        snprintf(vm->error, sizeof(vm->error), "%%pop-wind: wind stack underflow");
        return CH_UNDEFINED;
    }
    vm->wind_count--;
    return CH_VOID;
}

static ChValue prim_wind_top_after(ChVM *vm, ChValue *args, int nargs) {
    (void)args;
    (void)nargs;
    if (vm->wind_count == 0) {
        snprintf(vm->error, sizeof(vm->error), "%%wind-top-after: wind stack empty");
        return CH_UNDEFINED;
    }
    return vm->wind_stack[vm->wind_count - 1].after;
}

static ChValue prim_with_exception_handler(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    ChValue handler = args[0];
    ChValue thunk = args[1];
    if (!ch_is_procedure(handler) || !ch_is_procedure(thunk)) {
        snprintf(vm->error, sizeof(vm->error),
                 "with-exception-handler: arguments must be procedures");
        return CH_UNDEFINED;
    }
    if (ch_vm_ensure_handler_capacity(vm, vm->handler_count + 1) != 0) {
        return CH_UNDEFINED;
    }
    vm->handler_stack[vm->handler_count].handler = handler;
    vm->handler_stack[vm->handler_count].frame_count = vm->frame_count;
    vm->handler_stack[vm->handler_count].wind_count = vm->wind_count;
    vm->handler_count++;

    ChValue result = CH_VOID;
    ChVMStatus st = ch_vm_apply(vm, thunk, NULL, 0, &result);
    if (st == CH_VM_CONTINUATION_INVOKED) {
        vm->continuation_invoked = true;
        return CH_UNDEFINED;
    }
    /* Same park discipline as call/ec: do not pop the handler when the
     * thunk suspended — the fiber snapshot already captured it, and a
     * pop here would race the live VM stack before restore. */
    if (st != CH_VM_FIBER_PARKED && vm->handler_count > 0) {
        vm->handler_count--;
    }
    return finish_apply(vm, st, result);
}

static ChValue prim_raise(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    return ch_vm_raise(vm, args[0], 0);
}

static ChValue prim_raise_continuable(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    return ch_vm_raise(vm, args[0], 1);
}

/* Kaappi-style Scheme dynamic-wind over %push-wind / %pop-wind.
 * Validates all three arguments before running `before` so a bad-argument call
 * cannot leak before's side effects (#1375). Captures %-helpers as upvalues so
 * they can later be removed from the global namespace. `after` is recovered from
 * the wind stack via %wind-top-after so a register clobber during thunk
 * (continuation restore / call frame reuse) cannot lose the after thunk. */
static const char *dynamic_wind_src =
    "(define dynamic-wind\n"
    "  (let ((%push-wind %push-wind) (%pop-wind %pop-wind)\n"
    "        (%wind-top-after %wind-top-after)\n"
    "        (procedure? procedure?) (not not) (error error))\n"
    "    (lambda (before thunk after)\n"
    "      (if (not (procedure? before))\n"
    "          (error \"type error in 'dynamic-wind': expected procedure\" before))\n"
    "      (if (not (procedure? thunk))\n"
    "          (error \"type error in 'dynamic-wind': expected procedure\" thunk))\n"
    "      (if (not (procedure? after))\n"
    "          (error \"type error in 'dynamic-wind': expected procedure\" after))\n"
    "      (before)\n"
    "      (%push-wind before after)\n"
    "      (let ((result (thunk))\n"
    "            (after-thunk (%wind-top-after)))\n"
    "        (%pop-wind)\n"
    "        (after-thunk)\n"
    "        result))))\n";

static void undefine_global_cstr(ChVM *vm, const char *name) {
    ChValue sym = ch_gc_intern_symbol_cstr(&vm->gc, name);
    int idx = ch_vm_intern_global(vm, ch_as_symbol(sym));
    ch_vm_undefine_global(vm, idx);
}

void ch_hide_control_internal_helpers(ChVM *vm) {
    /* Captured by dynamic-wind's closure; must not remain globally callable. */
    undefine_global_cstr(vm, "%push-wind");
    undefine_global_cstr(vm, "%pop-wind");
    undefine_global_cstr(vm, "%wind-top-after");
}

void ch_install_control_bootstrap(ChVM *vm) {
    /* Needs error / procedure? already registered (after error primitives). */
    if (ch_eval_source(vm, dynamic_wind_src, strlen(dynamic_wind_src), 0) != 0) {
        fprintf(stderr, "chaaya: failed to install dynamic-wind bootstrap\n");
        abort();
    }
}

void ch_register_control_primitives(ChVM *vm) {
    define_prim(vm, "call/cc", prim_call_cc, 1, 1);
    define_prim(vm, "call-with-current-continuation", prim_call_cc, 1, 1);
    define_prim(vm, "call/ec", prim_call_ec, 1, 1);
    define_prim(vm, "call-with-escape-continuation", prim_call_ec, 1, 1);
    define_prim(vm, "%push-wind", prim_push_wind, 2, 2);
    define_prim(vm, "%pop-wind", prim_pop_wind, 0, 0);
    define_prim(vm, "%wind-top-after", prim_wind_top_after, 0, 0);
    define_prim(vm, "with-exception-handler", prim_with_exception_handler, 2, 2);
    define_prim(vm, "raise", prim_raise, 1, 1);
    define_prim(vm, "raise-continuable", prim_raise_continuable, 1, 1);
}
