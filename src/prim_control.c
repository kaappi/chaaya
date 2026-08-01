#include "chaaya/prim.h"

#include "chaaya/eval.h"
#include "chaaya/printer.h"

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
    ChValue cont = ch_vm_capture_continuation(vm, vm->native_result_slot);
    ChValue call_args[1] = {cont};
    ChValue result = CH_VOID;
    return finish_apply(vm, ch_vm_apply(vm, receiver, call_args, 1, &result), result);
}

static ChValue prim_push_wind(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    if (vm->wind_count >= CH_VM_MAX_WINDS) {
        snprintf(vm->error, sizeof(vm->error), "%%push-wind: wind stack overflow");
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

static ChValue prim_with_exception_handler(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    ChValue handler = args[0];
    ChValue thunk = args[1];
    if (!ch_is_procedure(handler) || !ch_is_procedure(thunk)) {
        snprintf(vm->error, sizeof(vm->error),
                 "with-exception-handler: arguments must be procedures");
        return CH_UNDEFINED;
    }
    if (vm->handler_count >= CH_VM_MAX_HANDLERS) {
        snprintf(vm->error, sizeof(vm->error), "with-exception-handler: handler stack overflow");
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
    if (vm->handler_count > 0) {
        vm->handler_count--;
    }
    return finish_apply(vm, st, result);
}

static ChValue raise_common(ChVM *vm, ChValue obj, int continuable) {
    if (vm->handler_count == 0) {
        char *printed = ch_value_to_string(obj, false);
        snprintf(vm->error, sizeof(vm->error), "uncaught exception: %s",
                 printed ? printed : "#<unknown>");
        free(printed);
        return CH_UNDEFINED;
    }

    ChExceptionHandler eh = vm->handler_stack[vm->handler_count - 1];
    if (!continuable) {
        vm->handler_count--;
        while (vm->wind_count > eh.wind_count) {
            vm->wind_count--;
            ChValue ignored = CH_VOID;
            ChVMStatus st =
                ch_vm_apply(vm, vm->wind_stack[vm->wind_count].after, NULL, 0, &ignored);
            if (st == CH_VM_CONTINUATION_INVOKED) {
                vm->continuation_invoked = true;
                return CH_UNDEFINED;
            }
        }
    }

    ChValue call_args[1] = {obj};
    ChValue result = CH_VOID;
    ChVMStatus st = ch_vm_apply(vm, eh.handler, call_args, 1, &result);
    if (st != CH_VM_OK) {
        return finish_apply(vm, st, result);
    }
    if (!continuable) {
        snprintf(vm->error, sizeof(vm->error),
                 "exception handler returned (non-continuable exception)");
        return CH_UNDEFINED;
    }
    return result;
}

static ChValue prim_raise(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    return raise_common(vm, args[0], 0);
}

static ChValue prim_raise_continuable(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    return raise_common(vm, args[0], 1);
}

static ChValue prim_error(ChVM *vm, ChValue *args, int nargs) {
    ChValue msg = nargs >= 1 ? args[0] : CH_NIL;
    ChValue irritants = CH_NIL;
    ch_gc_push(&vm->gc, &msg);
    ch_gc_push(&vm->gc, &irritants);
    for (int i = nargs - 1; i >= 1; i--) {
        ChValue item = args[i];
        ch_gc_push(&vm->gc, &item);
        irritants = ch_gc_cons(&vm->gc, item, irritants);
        ch_gc_pop(&vm->gc);
    }
    ChValue err = ch_gc_cons(&vm->gc, msg, irritants);
    ch_gc_pop_n(&vm->gc, 2);
    return raise_common(vm, err, 0);
}

/* Kaappi-style Scheme dynamic-wind over %push-wind / %pop-wind. */
static const char *dynamic_wind_src =
    "(define dynamic-wind\n"
    "  (lambda (before thunk after)\n"
    "    (before)\n"
    "    (%push-wind before after)\n"
    "    (let ((result (thunk)))\n"
    "      (%pop-wind)\n"
    "      (after)\n"
    "      result)))\n";

void ch_register_control_primitives(ChVM *vm) {
    define_prim(vm, "call/cc", prim_call_cc, 1, 1);
    define_prim(vm, "call-with-current-continuation", prim_call_cc, 1, 1);
    define_prim(vm, "%push-wind", prim_push_wind, 2, 2);
    define_prim(vm, "%pop-wind", prim_pop_wind, 0, 0);
    define_prim(vm, "with-exception-handler", prim_with_exception_handler, 2, 2);
    define_prim(vm, "raise", prim_raise, 1, 1);
    define_prim(vm, "raise-continuable", prim_raise_continuable, 1, 1);
    define_prim(vm, "error", prim_error, -1, 1);

    /* Install Scheme dynamic-wind (must run after %push-wind / %pop-wind). */
    if (ch_eval_source(vm, dynamic_wind_src, strlen(dynamic_wind_src), 0) != 0) {
        fprintf(stderr, "chaaya: failed to install dynamic-wind bootstrap\n");
        abort();
    }
}
