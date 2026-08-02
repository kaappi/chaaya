#include "chaaya/vm.h"

#include "chaaya/coverage.h"
#include "chaaya/profile.h"

#include "chaaya/features.h"
#include "chaaya/fiber.h"
#include "chaaya/ffi.h"
#include "chaaya/library.h"
#include "chaaya/thread.h"
#include "chaaya/opcode.h"
#include "chaaya/printer.h"
#include "chaaya/environment.h"
#include "chaaya/prim.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void ch_vm_init(ChVM *vm) {
    memset(vm, 0, sizeof(*vm));
    ch_gc_init(&vm->gc);
    vm->gc.vm = vm;
    vm->regs = (ChValue *)calloc(CH_VM_MAX_REGS, sizeof(ChValue));
    vm->frames = (ChCallFrame *)calloc(CH_VM_MAX_FRAMES, sizeof(ChCallFrame));
    if (!vm->regs || !vm->frames) {
        free(vm->regs);
        free(vm->frames);
        vm->regs = NULL;
        vm->frames = NULL;
        abort();
    }
    vm->result = CH_VOID;
    vm->default_random_source = CH_UNDEFINED;
    vm->ffi_callback_deferred_value = CH_UNDEFINED;
    vm->ffi_callback_raise_result = CH_UNDEFINED;
    vm->fiber_runtime = (ChFiberRuntime *)calloc(1, sizeof(ChFiberRuntime));
    if (vm->fiber_runtime) {
        ch_fiber_runtime_init(vm->fiber_runtime);
    }
    vm->libraries = (ChLibraryRegistry *)calloc(1, sizeof(ChLibraryRegistry));
    if (vm->libraries) {
        ch_library_registry_init(vm->libraries);
    }
    vm->owns_globals = true;
    vm->current_thread = CH_NIL;
    ch_thread_runtime_init(vm);
}

void ch_vm_deinit(ChVM *vm) {
    ChUpvalue *uv = vm->open_upvalues;
    while (uv) {
        ChUpvalue *next = uv->next;
        free(uv);
        uv = next;
    }
    if (vm->libraries) {
        ch_library_registry_deinit(vm->libraries);
        free(vm->libraries);
        vm->libraries = NULL;
    }
    for (size_t i = 0; i < vm->loading_lib_count; i++) {
        free(vm->loading_libs[i]);
    }
    free(vm->current_lib_dir);
    for (size_t i = 0; i < vm->syntax_prop_count; i++) {
        free(vm->syntax_props[i].key);
    }
    ch_thread_runtime_deinit(vm);
    if (vm->fiber_runtime) {
        ch_fiber_runtime_deinit(vm->fiber_runtime);
        free(vm->fiber_runtime);
        vm->fiber_runtime = NULL;
    }
    free(vm->regs);
    vm->regs = NULL;
    free(vm->frames);
    vm->frames = NULL;
    ch_gc_deinit(&vm->gc);
}

static int find_global(ChVM *vm, ChSymbol *sym) {
    for (size_t i = 0; i < vm->global_count; i++) {
        if (vm->globals[i].name == sym) {
            return (int)i;
        }
    }
    return -1;
}

int ch_vm_intern_global(ChVM *vm, ChSymbol *sym) {
    int idx = find_global(vm, sym);
    if (idx >= 0) {
        return idx;
    }
    if (vm->global_count >= CH_VM_MAX_GLOBALS) {
        abort();
    }
    idx = (int)vm->global_count++;
    vm->globals[idx].name = sym;
    vm->globals[idx].value = CH_UNDEFINED;
    vm->globals[idx].defined = false;
    return idx;
}

void ch_vm_define_global(ChVM *vm, int idx, ChValue v) {
    vm->globals[idx].value = v;
    vm->globals[idx].defined = true;
}

void ch_vm_register_primitives(ChVM *vm) {
    ch_register_core_primitives(vm);
    ch_register_list_primitives(vm);
    ch_register_data_primitives(vm);
    ch_register_char_primitives(vm);
    ch_register_string_primitives(vm);
    ch_register_vector_primitives(vm);
    ch_register_bytevector_primitives(vm);
    ch_register_math_primitives(vm);
    ch_register_port_primitives(vm);
    ch_register_control_primitives(vm);
    ch_register_error_primitives(vm);
    ch_register_record_primitives(vm);
    ch_register_lazy_primitives(vm);
    ch_register_eval_primitives(vm);
    ch_register_process_primitives(vm);
    ch_register_hashtable_primitives(vm);
    ch_register_fiber_primitives(vm);
#if !defined(__wasi__)
    ch_register_ffi_primitives(vm);
#endif
    ch_register_random_primitives(vm);
    ch_register_filesystem_primitives(vm);
    ch_register_weak_primitives(vm);
    ch_register_features_primitives(vm);
    ch_register_srfi1_primitives(vm);
    ch_register_srfi13_primitives(vm);
    ch_register_srfi133_primitives(vm);
    ch_register_srfi258_primitives(vm);
    ch_register_srfi260_primitives(vm);
    /* Scheme map/for-each overwrite native stubs (no C re-entrancy). */
    ch_install_list_bootstrap(vm);
    if (vm->libraries) {
        (void)ch_register_builtin_libraries(vm);
    }
}

const char *ch_vm_error(const ChVM *vm) {
    return vm->error;
}

static ChVMStatus runtime_error(ChVM *vm, const char *msg) {
    snprintf(vm->error, sizeof(vm->error), "%s", msg);
    vm->error_code = ch_diag_classify_message(msg, CH_DIAG_STAGE_RUNTIME);
    return CH_VM_RUNTIME_ERROR;
}

static ChVMStatus raise_message_to_slot(ChVM *vm, const char *msg, size_t result_slot) {
    if (vm->handler_count == 0) {
        snprintf(vm->error, sizeof(vm->error), "%s", msg);
        return CH_VM_RUNTIME_ERROR;
    }
    vm->error[0] = '\0';
    ChValue msgstr = ch_gc_make_string_cstr(&vm->gc, msg);
    ch_gc_push(&vm->gc, &msgstr);
    ChValue err = ch_gc_make_error_object(&vm->gc, msgstr, CH_NIL, 0);
    ch_gc_pop(&vm->gc);
    ChValue result = ch_vm_raise(vm, err, 0);
    if (vm->continuation_invoked) {
        return CH_VM_CONTINUATION_INVOKED;
    }
    if (vm->error[0] != '\0') {
        return CH_VM_RUNTIME_ERROR;
    }
    vm->regs[result_slot] = result;
    if (vm->frame_count == 0) {
        vm->result = result;
    }
    return CH_VM_OK;
}

static ChVMStatus raise_unbound(ChVM *vm, const char *name, uint8_t dst, ChValue *regs) {
    char buf[256];
    snprintf(buf, sizeof(buf), "unbound variable: %s", name);
    if (vm->handler_count == 0) {
        snprintf(vm->error, sizeof(vm->error), "%s", buf);
        return CH_VM_RUNTIME_ERROR;
    }
    vm->error[0] = '\0';
    ChValue msg = ch_gc_make_string_cstr(&vm->gc, buf);
    ch_gc_push(&vm->gc, &msg);
    ChValue err = ch_gc_make_error_object(&vm->gc, msg, CH_NIL, 0);
    ch_gc_pop(&vm->gc);
    ChValue result = ch_vm_raise(vm, err, 0);
    /* Guard escapes with call/cc — must not resume after the unbound load. */
    if (vm->continuation_invoked) {
        return CH_VM_CONTINUATION_INVOKED;
    }
    if (vm->error[0] != '\0') {
        return CH_VM_RUNTIME_ERROR;
    }
    regs[dst] = result;
    return CH_VM_OK;
}

ChValue ch_vm_parameter_ref(ChVM *vm, ChValue parameter) {
    for (size_t i = vm->parameter_count; i > 0; i--) {
        size_t idx = i - 1;
        if (vm->parameter_stack[idx].parameter == parameter) {
            return vm->parameter_stack[idx].value;
        }
    }
    return ch_as_parameter(parameter)->value;
}

int ch_vm_parameter_set(ChVM *vm, ChValue parameter, ChValue value) {
    for (size_t i = vm->parameter_count; i > 0; i--) {
        size_t idx = i - 1;
        if (vm->parameter_stack[idx].parameter == parameter) {
            vm->parameter_stack[idx].value = value;
            return 0;
        }
    }
    ChParameter *param = ch_as_parameter(parameter);
    param->value = value;
    ch_gc_write_barrier(&vm->gc, &param->header, value);
    return 0;
}

int ch_vm_parameter_push(ChVM *vm, ChValue parameter, ChValue value) {
    if (vm->parameter_count >= CH_VM_MAX_PARAMETER_BINDINGS) {
        snprintf(vm->error, sizeof(vm->error), "parameter stack overflow");
        return -1;
    }
    vm->parameter_stack[vm->parameter_count].parameter = parameter;
    vm->parameter_stack[vm->parameter_count].value = value;
    vm->parameter_count++;
    return 0;
}

int ch_vm_parameter_pop(ChVM *vm, ChValue parameter) {
    if (vm->parameter_count == 0) {
        snprintf(vm->error, sizeof(vm->error), "parameter stack underflow");
        return -1;
    }
    size_t top = vm->parameter_count - 1;
    if (vm->parameter_stack[top].parameter != parameter) {
        snprintf(vm->error, sizeof(vm->error), "parameter stack mismatch");
        return -1;
    }
    vm->parameter_count--;
    return 0;
}

ChValue ch_vm_raise(ChVM *vm, ChValue obj, int continuable) {
    if (vm->ffi_callback_depth > 0) {
        if (!vm->ffi_callback_deferred) {
            vm->ffi_callback_deferred = true;
            vm->ffi_callback_deferred_value = obj;
        }
        return CH_UNDEFINED;
    }
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
            if (st != CH_VM_OK) {
                return CH_UNDEFINED;
            }
        }
    }

    ChValue call_args[1] = {obj};
    ChValue result = CH_VOID;
    size_t handlers_before = vm->handler_count;
    ChVMStatus st = ch_vm_apply(vm, eh.handler, call_args, 1, &result);
    /* Guard escapes via call/cc: apply may report OK at its barrier while a
     * parent continuation actually delivered the value (continuation_invoked),
     * or restore a different handler stack (nested guard re-raise). */
    if (st == CH_VM_CONTINUATION_INVOKED || vm->continuation_invoked ||
        vm->handler_count != handlers_before) {
        vm->continuation_invoked = true;
        return CH_UNDEFINED;
    }
    if (st != CH_VM_OK) {
        return CH_UNDEFINED;
    }
    if (!continuable) {
        /* R7RS 6.11: returning from a non-continuable raise handler raises a
         * secondary exception (the original handler was already popped). */
        ChValue msg =
            ch_gc_make_string_cstr(&vm->gc, "exception handler returned from non-continuable exception");
        ch_gc_push(&vm->gc, &msg);
        ChValue secondary = ch_gc_make_error_object(&vm->gc, msg, CH_NIL, 0);
        ch_gc_push(&vm->gc, &secondary);
        ChValue raised = ch_vm_raise(vm, secondary, 0);
        ch_gc_pop_n(&vm->gc, 2);
        return raised;
    }
    return result;
}

static uint8_t read_u8(ChCallFrame *frame) {
    return *frame->ip++;
}

static uint16_t read_u16(ChCallFrame *frame) {
    uint16_t lo = read_u8(frame);
    uint16_t hi = read_u8(frame);
    return (uint16_t)(lo | (hi << 8));
}

static int16_t read_i16(ChCallFrame *frame) {
    return (int16_t)read_u16(frame);
}

static ChValue *frame_regs(ChVM *vm, ChCallFrame *frame) {
    return &vm->regs[frame->reg_base];
}

static ChLibEnv *resolve_lib_env(ChVM *vm, ChCallFrame *frame) {
    if (vm->active_lib_env) {
        return vm->active_lib_env;
    }
    if (frame && frame->closure && frame->closure->home_env) {
        return frame->closure->home_env;
    }
    return NULL;
}

size_t ch_vm_push_gc_roots(ChVM *vm) {
    /* Registers, frames, upvalues, globals, etc. are marked in
     * ch_vm_mark_gc_roots during collection (gc->vm). Pushing the full
     * register window here exhausts CH_GC_ROOT_MAX on deep Scheme recursion
     * (map callbacks, etc.). Explicit ch_gc_push remains for C locals. */
    (void)vm;
    return 0;
}

void ch_vm_mark_gc_roots(ChVM *vm) {
    for (size_t i = 0; i < vm->reg_top; i++) {
        ch_gc_mark_value(vm->regs[i]);
    }
    for (size_t i = 0; i < vm->global_count; i++) {
        ch_gc_mark_value(vm->globals[i].value);
    }
    /* In-construction define-library env is not yet in vm->libraries. */
    if (vm->active_lib_env) {
        for (size_t i = 0; i < vm->active_lib_env->count; i++) {
            if (vm->active_lib_env->bindings[i].defined) {
                ch_gc_mark_value(vm->active_lib_env->bindings[i].value);
            }
        }
    }
    for (size_t i = 0; i < vm->wind_count; i++) {
        ch_gc_mark_value(vm->wind_stack[i].before);
        ch_gc_mark_value(vm->wind_stack[i].after);
    }
    for (size_t i = 0; i < vm->handler_count; i++) {
        ch_gc_mark_value(vm->handler_stack[i].handler);
    }
    for (size_t i = 0; i < vm->parameter_count; i++) {
        ch_gc_mark_value(vm->parameter_stack[i].parameter);
        ch_gc_mark_value(vm->parameter_stack[i].value);
    }
    for (size_t i = 0; i < vm->macro_count; i++) {
        ch_gc_mark_value(vm->macros[i].transformer);
    }
    for (ChUpvalue *uv = vm->open_upvalues; uv; uv = uv->next) {
        if (uv->is_closed) {
            ch_gc_mark_value(uv->closed_value);
        }
    }
    for (size_t i = 0; i < vm->frame_count; i++) {
        if (vm->frames[i].closure) {
            ch_gc_mark_value(ch_make_pointer(&vm->frames[i].closure->header));
        }
    }
    if (vm->has_pending_call) {
        ch_gc_mark_value(vm->pending_proc);
        for (int i = 0; i < vm->pending_nargs; i++) {
            ch_gc_mark_value(vm->pending_args[i]);
        }
    }
    if (vm->fiber_runtime) {
        ChFiberRuntime *rt = vm->fiber_runtime;
        for (size_t i = 0; i < rt->ready_count; i++) {
            size_t idx = (rt->ready_head + i) % CH_FIBER_READY_MAX;
            ch_gc_mark_value(rt->ready[idx]);
        }
        ch_gc_mark_value(rt->current);
        ch_gc_mark_value(vm->current_thread);
        for (size_t i = 0; i < CH_REACTOR_MAX_TIMERS; i++) {
            if (rt->reactor.timers[i].active) {
                ch_gc_mark_value(rt->reactor.timers[i].payload);
            }
        }
        for (size_t i = 0; i < CH_REACTOR_MAX_FDS; i++) {
            if (rt->reactor.fds[i].active) {
                ch_gc_mark_value(rt->reactor.fds[i].payload);
            }
        }
    }
    if (vm->ffi_callback_deferred_value != CH_UNDEFINED) {
        ch_gc_mark_value(vm->ffi_callback_deferred_value);
    }
    if (vm->ffi_callback_raise_result != CH_UNDEFINED) {
        ch_gc_mark_value(vm->ffi_callback_raise_result);
    }
}

static size_t push_gc_roots(ChVM *vm) {
    return ch_vm_push_gc_roots(vm);
}

static void pop_gc_roots_n(ChVM *vm, size_t n) {
    ch_gc_pop_n(&vm->gc, n);
}

static void close_upvalues(ChVM *vm, ChValue *last) {
    while (vm->open_upvalues && vm->open_upvalues->location >= last) {
        ChUpvalue *uv = vm->open_upvalues;
        uv->closed_value = *uv->location;
        uv->location = &uv->closed_value;
        uv->is_closed = true;
        vm->open_upvalues = uv->next;
    }
}

static void close_all_open_upvalues(ChVM *vm) {
    while (vm->open_upvalues) {
        ChUpvalue *uv = vm->open_upvalues;
        if (!uv->is_closed) {
            uv->closed_value = *uv->location;
            uv->location = &uv->closed_value;
            uv->is_closed = true;
        }
        vm->open_upvalues = uv->next;
    }
}

static ChUpvalue *capture_upvalue(ChVM *vm, ChValue *local) {
    ChUpvalue *prev = NULL;
    ChUpvalue *uv = vm->open_upvalues;
    while (uv && uv->location > local) {
        prev = uv;
        uv = uv->next;
    }
    if (uv && uv->location == local) {
        return uv;
    }
    ChUpvalue *created = (ChUpvalue *)calloc(1, sizeof(ChUpvalue));
    if (!created) {
        abort();
    }
    created->location = local;
    created->is_closed = false;
    created->next = uv;
    if (prev) {
        prev->next = created;
    } else {
        vm->open_upvalues = created;
    }
    return created;
}

static ChVMStatus push_frame(ChVM *vm, ChClosure *closure, size_t reg_base) {
    if (vm->frame_count >= CH_VM_MAX_FRAMES) {
        return CH_VM_STACK_OVERFLOW;
    }
    ChCallFrame *frame = &vm->frames[vm->frame_count++];
    frame->closure = closure;
    frame->ip = closure->fn->code;
    frame->reg_base = reg_base;
    frame->num_regs = closure->fn->num_regs;
    size_t need = reg_base + closure->fn->num_regs;
    if (need > CH_VM_MAX_REGS) {
        return CH_VM_STACK_OVERFLOW;
    }
    if (need > vm->reg_top) {
        for (size_t i = vm->reg_top; i < need; i++) {
            vm->regs[i] = CH_UNDEFINED;
        }
        vm->reg_top = need;
    }
    return CH_VM_OK;
}

static ChVMStatus call_value(ChVM *vm, ChValue callee, size_t arg_base, int nargs, bool tail);
static ChVMStatus run_until(ChVM *vm, size_t target_frames);

static size_t compute_reg_top(const ChVM *vm) {
    size_t top = 0;
    for (size_t i = 0; i < vm->frame_count; i++) {
        const ChCallFrame *f = &vm->frames[i];
        size_t end = f->reg_base + f->num_regs;
        if (end > top) {
            top = end;
        }
    }
    return top;
}

void ch_vm_reclaim_regs(ChVM *vm) {
    vm->reg_top = compute_reg_top(vm);
}

/* calloc that never returns NULL; allocates at least one element. */
static void *alloc_or_abort(size_t count, size_t size) {
    void *p = calloc(count == 0 ? 1 : count, size);
    if (!p) {
        abort();
    }
    return p;
}

static ChVMStatus apply_thunk(ChVM *vm, ChValue thunk) {
    ChValue ignored = CH_VOID;
    return ch_vm_apply(vm, thunk, NULL, 0, &ignored);
}

/* After a top-level continuation restore, keep running until the stack empties. */
static ChVMStatus resume_after_continuation(ChVM *vm, ChVMStatus st) {
    if (st == CH_VM_CONTINUATION_INVOKED) {
        /* Value already in vm->result from invoke_continuation. If frames
         * remain, drain them; if already empty, that value is the answer. */
        if (vm->frame_count == 0) {
            return CH_VM_OK;
        }
        return run_until(vm, 0);
    }
    return st;
}

ChValue ch_vm_capture_continuation(ChVM *vm, size_t result_slot) {
    size_t max_reg = vm->reg_top;
    if (result_slot + 1 > max_reg) {
        max_reg = result_slot + 1;
    }

    size_t roots = push_gc_roots(vm);
    ChValue cont_v = ch_gc_make_continuation(&vm->gc);
    ch_gc_push(&vm->gc, &cont_v);
    ChContinuation *c = ch_as_continuation(cont_v);

    c->register_count = max_reg;
    c->registers = (ChValue *)alloc_or_abort(max_reg, sizeof(ChValue));
    memcpy(c->registers, vm->regs, max_reg * sizeof(ChValue));

    c->frame_count = vm->frame_count;
    c->frames = (ChSavedFrame *)alloc_or_abort(vm->frame_count, sizeof(ChSavedFrame));
    for (size_t i = 0; i < vm->frame_count; i++) {
        ChCallFrame *f = &vm->frames[i];
        c->frames[i].closure = f->closure;
        c->frames[i].ip_offset = (size_t)(f->ip - f->closure->fn->code);
        c->frames[i].reg_base = f->reg_base;
        c->frames[i].num_regs = f->num_regs;
    }

    c->wind_count = vm->wind_count;
    c->winds = (ChWindRecord *)alloc_or_abort(vm->wind_count, sizeof(ChWindRecord));
    memcpy(c->winds, vm->wind_stack, vm->wind_count * sizeof(ChWindRecord));

    c->handler_count = vm->handler_count;
    c->handlers = (ChExceptionHandler *)alloc_or_abort(vm->handler_count, sizeof(ChExceptionHandler));
    memcpy(c->handlers, vm->handler_stack, vm->handler_count * sizeof(ChExceptionHandler));

    c->parameter_binding_count = vm->parameter_count;
    c->parameter_bindings =
        (ChParameterBinding *)alloc_or_abort(vm->parameter_count, sizeof(ChParameterBinding));
    memcpy(c->parameter_bindings, vm->parameter_stack,
           vm->parameter_count * sizeof(ChParameterBinding));

    size_t nuv = 0;
    for (ChUpvalue *uv = vm->open_upvalues; uv; uv = uv->next) {
        nuv++;
    }
    c->open_uv_count = nuv;
    c->open_uvs = (ChSavedUpvalue *)alloc_or_abort(nuv, sizeof(ChSavedUpvalue));
    size_t ui = 0;
    for (ChUpvalue *uv = vm->open_upvalues; uv; uv = uv->next) {
        c->open_uvs[ui].uv = uv;
        c->open_uvs[ui].reg_index = (size_t)(uv->location - vm->regs);
        ui++;
    }

    c->result_slot = result_slot;
    ch_gc_pop(&vm->gc);
    pop_gc_roots_n(vm, roots);
    return cont_v;
}

ChVMStatus ch_vm_wind_transition(ChVM *vm, const ChWindRecord *target, size_t target_count) {
    size_t min_len = vm->wind_count < target_count ? vm->wind_count : target_count;
    size_t common = 0;
    while (common < min_len &&
           ch_eq(vm->wind_stack[common].before, target[common].before) &&
           ch_eq(vm->wind_stack[common].after, target[common].after)) {
        common++;
    }

    while (vm->wind_count > common) {
        vm->wind_count--;
        ChVMStatus st = apply_thunk(vm, vm->wind_stack[vm->wind_count].after);
        if (st != CH_VM_OK) {
            return st;
        }
    }

    for (size_t j = common; j < target_count; j++) {
        ChVMStatus st = apply_thunk(vm, target[j].before);
        if (st != CH_VM_OK) {
            return st;
        }
        if (vm->wind_count >= CH_VM_MAX_WINDS) {
            return CH_VM_STACK_OVERFLOW;
        }
        vm->wind_stack[vm->wind_count++] = target[j];
    }
    return CH_VM_OK;
}

ChVMStatus ch_vm_invoke_continuation(ChVM *vm, ChContinuation *cont, ChValue value) {
    /* CONTINUATION_INVOKED: a wind thunk already restored another continuation. */
    ChVMStatus st = ch_vm_wind_transition(vm, cont->winds, cont->wind_count);
    if (st != CH_VM_OK) {
        return st;
    }

    close_all_open_upvalues(vm);

    if (cont->register_count > CH_VM_MAX_REGS || cont->frame_count > CH_VM_MAX_FRAMES ||
        cont->wind_count > CH_VM_MAX_WINDS || cont->handler_count > CH_VM_MAX_HANDLERS ||
        cont->parameter_binding_count > CH_VM_MAX_PARAMETER_BINDINGS) {
        return CH_VM_STACK_OVERFLOW;
    }
    if (cont->result_slot >= CH_VM_MAX_REGS) {
        return CH_VM_STACK_OVERFLOW;
    }

    memcpy(vm->regs, cont->registers, cont->register_count * sizeof(ChValue));
    vm->reg_top = cont->register_count;

    for (size_t i = 0; i < cont->frame_count; i++) {
        ChSavedFrame *sf = &cont->frames[i];
        ChCallFrame *f = &vm->frames[i];
        f->closure = sf->closure;
        f->ip = sf->closure->fn->code + sf->ip_offset;
        f->reg_base = sf->reg_base;
        f->num_regs = sf->num_regs;
    }
    vm->frame_count = cont->frame_count;

    memcpy(vm->handler_stack, cont->handlers, cont->handler_count * sizeof(ChExceptionHandler));
    vm->handler_count = cont->handler_count;

    memcpy(vm->parameter_stack, cont->parameter_bindings,
           cont->parameter_binding_count * sizeof(ChParameterBinding));
    vm->parameter_count = cont->parameter_binding_count;

    /* Re-sync from snapshot in case before/after thunks mutated the wind stack. */
    memcpy(vm->wind_stack, cont->winds, cont->wind_count * sizeof(ChWindRecord));
    vm->wind_count = cont->wind_count;

    /* close_all_open_upvalues (above) saved live set! mutations into closed_value.
     * Prefer those over the register snapshot so mutable bindings persist across
     * continuation re-entry (R7RS / assignment semantics), then reopen. */
    vm->open_upvalues = NULL;
    for (size_t i = 0; i < cont->open_uv_count; i++) {
        ChUpvalue *uv = cont->open_uvs[i].uv;
        size_t idx = cont->open_uvs[i].reg_index;
        if (idx >= vm->reg_top) {
            continue;
        }
        if (uv->is_closed) {
            vm->regs[idx] = uv->closed_value;
        }
        uv->location = &vm->regs[idx];
        uv->is_closed = false;
        uv->next = vm->open_upvalues;
        vm->open_upvalues = uv;
    }

    vm->regs[cont->result_slot] = value;
    /* Top-level / barrier resumes read vm->result when frames are empty. */
    vm->result = value;
    vm->continuation_invoked = true;
    return CH_VM_CONTINUATION_INVOKED;
}

static ChVMStatus run_until(ChVM *vm, size_t target_frames) {
    for (;;) {
        if (vm->frame_count <= target_frames) {
            return CH_VM_OK;
        }
        ChCallFrame *frame = &vm->frames[vm->frame_count - 1];
        ChValue *regs = frame_regs(vm, frame);
        ChOpCode op = (ChOpCode)read_u8(frame);

        switch (op) {
        case CH_OP_LOAD_CONST: {
            uint8_t dst = read_u8(frame);
            uint16_t idx = read_u16(frame);
            regs[dst] = frame->closure->fn->constants[idx];
            break;
        }
        case CH_OP_LOAD_NIL:
            regs[read_u8(frame)] = CH_NIL;
            break;
        case CH_OP_LOAD_TRUE:
            regs[read_u8(frame)] = CH_TRUE;
            break;
        case CH_OP_LOAD_FALSE:
            regs[read_u8(frame)] = CH_FALSE;
            break;
        case CH_OP_LOAD_VOID:
            regs[read_u8(frame)] = CH_VOID;
            break;
        case CH_OP_MOVE: {
            uint8_t dst = read_u8(frame);
            uint8_t src = read_u8(frame);
            regs[dst] = regs[src];
            break;
        }
        case CH_OP_GET_GLOBAL: {
            uint8_t dst = read_u8(frame);
            uint16_t idx = read_u16(frame);
            if (idx & CH_ENV_LIB_BIT) {
                uint16_t li = (uint16_t)(idx & ~CH_ENV_LIB_BIT);
                ChLibEnv *env = resolve_lib_env(vm, frame);
                if (!env || li >= env->count || !env->bindings[li].defined) {
                    const char *name =
                        env && li < env->count ? env->bindings[li].name->name : "?";
                    ChVMStatus st = raise_unbound(vm, name, dst, regs);
                    if (st != CH_VM_OK) {
                        return st;
                    }
                    break;
                }
                regs[dst] = env->bindings[li].value;
            } else {
                if (!vm->globals[idx].defined) {
                    ChVMStatus st =
                        raise_unbound(vm, vm->globals[idx].name->name, dst, regs);
                    if (st != CH_VM_OK) {
                        return st;
                    }
                    break;
                }
                regs[dst] = vm->globals[idx].value;
            }
            break;
        }
        case CH_OP_SET_GLOBAL: {
            uint16_t idx = read_u16(frame);
            uint8_t src = read_u8(frame);
            if (idx & CH_ENV_LIB_BIT) {
                uint16_t li = (uint16_t)(idx & ~CH_ENV_LIB_BIT);
                ChLibEnv *env = resolve_lib_env(vm, frame);
                if (!env || li >= env->count || !env->bindings[li].defined) {
                    snprintf(vm->error, sizeof(vm->error), "unbound variable: %s",
                             env && li < env->count ? env->bindings[li].name->name : "?");
                    return CH_VM_RUNTIME_ERROR;
                }
                env->bindings[li].value = regs[src];
            } else {
                if (!vm->globals[idx].defined) {
                    snprintf(vm->error, sizeof(vm->error), "unbound variable: %s",
                             vm->globals[idx].name->name);
                    return CH_VM_RUNTIME_ERROR;
                }
                vm->globals[idx].value = regs[src];
            }
            break;
        }
        case CH_OP_DEFINE_GLOBAL: {
            uint16_t idx = read_u16(frame);
            uint8_t src = read_u8(frame);
            if (idx & CH_ENV_LIB_BIT) {
                uint16_t li = (uint16_t)(idx & ~CH_ENV_LIB_BIT);
                ChLibEnv *env = resolve_lib_env(vm, frame);
                if (!env || li >= env->count) {
                    return runtime_error(vm, "define: bad library binding");
                }
                env->bindings[li].value = regs[src];
                env->bindings[li].defined = true;
            } else {
                vm->globals[idx].value = regs[src];
                vm->globals[idx].defined = true;
            }
            break;
        }
        case CH_OP_GET_UPVALUE: {
            uint8_t dst = read_u8(frame);
            uint8_t idx = read_u8(frame);
            regs[dst] = *frame->closure->upvalues[idx]->location;
            break;
        }
        case CH_OP_SET_UPVALUE: {
            uint8_t idx = read_u8(frame);
            uint8_t src = read_u8(frame);
            *frame->closure->upvalues[idx]->location = regs[src];
            ch_gc_write_barrier(&vm->gc, &frame->closure->header, regs[src]);
            break;
        }
        case CH_OP_CONS: {
            uint8_t dst = read_u8(frame);
            uint8_t car = read_u8(frame);
            uint8_t cdr = read_u8(frame);
            size_t roots = push_gc_roots(vm);
            regs[dst] = ch_gc_cons(&vm->gc, regs[car], regs[cdr]);
            pop_gc_roots_n(vm, roots);
            break;
        }
        case CH_OP_JUMP: {
            int16_t off = read_i16(frame);
            frame->ip += off;
            break;
        }
        case CH_OP_JUMP_FALSE: {
            uint8_t test = read_u8(frame);
            int16_t off = read_i16(frame);
            if (!ch_is_true_value(regs[test])) {
                frame->ip += off;
            }
            break;
        }
        case CH_OP_JUMP_TRUE: {
            uint8_t test = read_u8(frame);
            int16_t off = read_i16(frame);
            if (ch_is_true_value(regs[test])) {
                frame->ip += off;
            }
            break;
        }
        case CH_OP_CLOSURE: {
            uint8_t dst = read_u8(frame);
            uint16_t idx = read_u16(frame);
            ChValue fn_v = frame->closure->fn->constants[idx];
            size_t roots = push_gc_roots(vm);
            ch_gc_push(&vm->gc, &fn_v);
            ChFunction *fn = ch_as_function(fn_v);
            ChUpvalue **uvs = NULL;
            if (fn->num_upvalues > 0) {
                uvs = (ChUpvalue **)calloc(fn->num_upvalues, sizeof(ChUpvalue *));
                if (!uvs) {
                    abort();
                }
                for (uint8_t i = 0; i < fn->num_upvalues; i++) {
                    uint8_t uidx = fn->uv_index[i];
                    if (fn->uv_is_local[i]) {
                        uvs[i] = capture_upvalue(vm, &regs[uidx]);
                    } else {
                        uvs[i] = frame->closure->upvalues[uidx];
                    }
                }
            }
            regs[dst] = ch_gc_make_closure(&vm->gc, ch_as_function(fn_v), uvs);
            {
                ChLibEnv *home = vm->active_lib_env;
                if (!home && frame->closure) {
                    home = frame->closure->home_env;
                }
                ch_as_closure(regs[dst])->home_env = home;
            }
            ch_gc_pop_n(&vm->gc, roots + 1);
            break;
        }
        case CH_OP_CALL:
        case CH_OP_TAIL_CALL: {
            uint8_t base = read_u8(frame);
            uint8_t nargs = read_u8(frame);
            ChVMStatus st =
                call_value(vm, regs[base], frame->reg_base + base, nargs, op == CH_OP_TAIL_CALL);
            if (st == CH_VM_FIBER_PARKED) {
                return CH_VM_FIBER_PARKED;
            }
            if (st == CH_VM_CONTINUATION_INVOKED) {
                /* Keep running if the restore is still above our barrier;
                 * otherwise propagate so ch_vm_apply can deliver regs[base]. */
                if (vm->frame_count <= target_frames) {
                    return CH_VM_CONTINUATION_INVOKED;
                }
                continue;
            }
            if (st != CH_VM_OK) {
                return st;
            }
            /* Discard extra values except values objects and tail context. */
            if (op == CH_OP_CALL && !ch_is_values(regs[base])) {
                regs[base] = ch_coerce_single(regs[base]);
            }
            break;
        }
        case CH_OP_RETURN: {
            uint8_t src = read_u8(frame);
            ChValue result = regs[src];
            close_upvalues(vm, regs);
            if (vm->frame_count == 1) {
                vm->result = ch_is_values(result) ? result : ch_coerce_single(result);
                vm->frame_count = 0;
                return CH_VM_OK;
            }
            size_t ret_slot = frame->reg_base;
            vm->frame_count--;
            /* Preserve multiple values for call-with-values (tail producer). */
            vm->regs[ret_slot] = result;
            if (vm->frame_count <= target_frames) {
                return CH_VM_OK;
            }
            break;
        }
        case CH_OP_HALT:
            vm->result = regs[0];
            close_upvalues(vm, regs);
            vm->frame_count = 0;
            return CH_VM_OK;
        default:
            (void)runtime_error(vm, "invalid opcode");
            unreachable();
        }
    }
}

static ChValue build_rest_list(ChVM *vm, ChValue *args, int start, int nargs) {
    ChValue list = CH_NIL;
    ch_gc_push(&vm->gc, &list);
    for (int i = nargs - 1; i >= start; i--) {
        ChValue item = args[i];
        ch_gc_push(&vm->gc, &item);
        list = ch_gc_cons(&vm->gc, item, list);
        ch_gc_pop(&vm->gc);
    }
    ch_gc_pop(&vm->gc);
    return list;
}

static ChVMStatus call_value(ChVM *vm, ChValue callee, size_t arg_base, int nargs, bool tail);

static const char *debug_callee_name(ChVM *vm, ChValue callee) {
    if (ch_is_native(callee)) {
        return ch_as_native(callee)->name;
    }
    for (size_t g = 0; g < vm->global_count; g++) {
        if (!vm->globals[g].defined) {
            continue;
        }
        if (ch_eqv(vm->globals[g].value, callee)) {
            return vm->globals[g].name->name;
        }
    }
    return NULL;
}

static void vm_debug_on_call(ChVM *vm, ChValue callee) {
    const char *name = debug_callee_name(vm, callee);
    bool hit = false;
    if (vm->step_trace || vm->debug_step_mode == 1) {
        fputs("; step ", stderr);
        ch_print_value(stderr, callee, false);
        fputc('\n', stderr);
        vm->step_trace = false;
        vm->debug_step_mode = 0;
        hit = true;
        if (!name) {
            name = "<anonymous>";
        }
    }
    if (!hit && vm->debug_mode && vm->breakpoint_count > 0 && name) {
        for (size_t b = 0; b < vm->breakpoint_count; b++) {
            if (strcmp(vm->breakpoints[b], name) == 0) {
                hit = true;
                break;
            }
        }
    }
    if (!hit) {
        return;
    }
    if (vm->debug_break_hook) {
        (void)vm->debug_break_hook(vm, name ? name : "<anonymous>");
    } else {
        fprintf(stderr, "; break at %s\n", name ? name : "<anonymous>");
    }
}

static ChValue pack_continuation_value(ChVM *vm, size_t arg_base, int nargs) {
    if (nargs == 1) {
        return vm->regs[arg_base + 1];
    }
    if (nargs == 0) {
        return CH_VOID;
    }
    return ch_gc_make_values(&vm->gc, &vm->regs[arg_base + 1], (size_t)nargs);
}

static ChVMStatus call_value(ChVM *vm, ChValue callee, size_t arg_base, int nargs, bool tail) {
    vm_debug_on_call(vm, callee);
    if (ch_is_continuation(callee)) {
        if (nargs < 0) {
            snprintf(vm->error, sizeof(vm->error), "continuation: bad argument count");
            return CH_VM_RUNTIME_ERROR;
        }
        ChValue value = pack_continuation_value(vm, arg_base, nargs);
        return ch_vm_invoke_continuation(vm, ch_as_continuation(callee), value);
    }

    if (ch_is_native(callee)) {
        ChNative *n = ch_as_native(callee);
        ChValue *args = &vm->regs[arg_base + 1];
        size_t result_slot = arg_base;
        bool was_tail = tail;
        if (tail && vm->frame_count > 0) {
            /* Drop current frame first so call/cc captures the caller (R7RS tail). */
            ChCallFrame *frame = &vm->frames[vm->frame_count - 1];
            close_upvalues(vm, frame_regs(vm, frame));
            result_slot = frame->reg_base;
            vm->frame_count--;
        }
        if (n->arity >= 0 && nargs != n->arity) {
            char buf[256];
            snprintf(buf, sizeof(buf), "%s: expected %d args, got %d", n->name, n->arity, nargs);
            return raise_message_to_slot(vm, buf, result_slot);
        }
        if (n->arity < 0 && nargs < n->min_arity) {
            char buf[256];
            snprintf(buf, sizeof(buf), "%s: expected at least %d args, got %d", n->name, n->min_arity,
                     nargs);
            return raise_message_to_slot(vm, buf, result_slot);
        }
        vm->native_result_slot = result_slot;
        bool saved_native_was_tail = vm->native_was_tail;
        vm->native_was_tail = was_tail;
        vm->continuation_invoked = false;
        vm->has_pending_call = false;
        if (n->name) {
            ch_profile_enter(n->name);
            ch_coverage_hit("native", n->name);
        }
        size_t roots = push_gc_roots(vm);
        ChValue result = n->fn(vm, args, nargs);
        pop_gc_roots_n(vm, roots);
        if (n->name) {
            ch_profile_leave(n->name);
        }
        vm->native_was_tail = saved_native_was_tail;
        if (vm->fiber_parked) {
            vm->has_pending_call = false;
            return CH_VM_FIBER_PARKED;
        }
        if (vm->continuation_invoked) {
            vm->continuation_invoked = false;
            /* A native may convert a barrier landing into a pending follow-up
             * (call-with-values → consumer). Prefer draining that over escape. */
            if (!vm->has_pending_call) {
                return CH_VM_CONTINUATION_INVOKED;
            }
        }
        if (vm->error[0] != '\0' && result == CH_UNDEFINED && !vm->has_pending_call) {
            char buf[256];
            snprintf(buf, sizeof(buf), "%s", vm->error);
            return raise_message_to_slot(vm, buf, result_slot);
        }
        /* Drain pending follow-up calls without nesting run_until (TCO).
         * The native's frame was already dropped when was_tail; install the
         * follow-up at result_slot with tail=false so we do not pop the real
         * caller (that bug broke call/cc return values). */
        while (vm->has_pending_call) {
            ChValue pproc = vm->pending_proc;
            int pnargs = vm->pending_nargs;
            ChValue pargs[CH_VM_MAX_PENDING_ARGS];
            for (int i = 0; i < pnargs; i++) {
                pargs[i] = vm->pending_args[i];
            }
            vm->has_pending_call = false;
            (void)vm->pending_call_tail;

            size_t base = result_slot;
            if (base + 1 + (size_t)pnargs > CH_VM_MAX_REGS) {
                return CH_VM_STACK_OVERFLOW;
            }
            if (vm->reg_top < base + 1 + (size_t)pnargs) {
                vm->reg_top = base + 1 + (size_t)pnargs;
            }
            vm->regs[base] = pproc;
            for (int i = 0; i < pnargs; i++) {
                vm->regs[base + 1 + (size_t)i] = pargs[i];
            }
            size_t frames_before = vm->frame_count;
            ChVMStatus st = call_value(vm, pproc, base, pnargs, false);
            if (st == CH_VM_CONTINUATION_INVOKED) {
                return st;
            }
            if (st != CH_VM_OK) {
                return st;
            }
            /* Closure frame pushed: let the outer run_until continue. */
            if (vm->frame_count > frames_before) {
                return CH_VM_OK;
            }
            /* Synchronous native completion — may have queued another pending. */
            result = vm->regs[base];
        }
        vm->regs[result_slot] = result;
        if (was_tail && vm->frame_count == 0) {
            vm->result = result;
        }
        return CH_VM_OK;
    }

    if (ch_is_foreign_procedure(callee)) {
        ChForeignProcedure *proc = ch_as_foreign_procedure(callee);
        ChValue *args = &vm->regs[arg_base + 1];
        size_t result_slot = arg_base;
        if (tail && vm->frame_count > 0) {
            ChCallFrame *frame = &vm->frames[vm->frame_count - 1];
            close_upvalues(vm, frame_regs(vm, frame));
            result_slot = frame->reg_base;
            vm->frame_count--;
        }
        ChValue result = CH_UNDEFINED;
        size_t roots = push_gc_roots(vm);
        int rc = ch_ffi_call(vm, proc, args, nargs, &result);
        pop_gc_roots_n(vm, roots);
        if (rc == 1) {
            vm->regs[result_slot] = vm->ffi_callback_raise_result;
            if (tail && vm->frame_count == 0) {
                vm->result = vm->ffi_callback_raise_result;
            }
            return CH_VM_OK;
        }
        if (rc != 0) {
            if (vm->error[0] != '\0' && vm->handler_count > 0) {
                char buf[256];
                snprintf(buf, sizeof(buf), "%s", vm->error);
                return raise_message_to_slot(vm, buf, result_slot);
            }
            return CH_VM_RUNTIME_ERROR;
        }
        vm->regs[result_slot] = result;
        if (tail && vm->frame_count == 0) {
            vm->result = result;
        }
        return CH_VM_OK;
    }

    if (ch_is_parameter(callee)) {
        if (nargs < 0 || nargs > 1) {
            snprintf(vm->error, sizeof(vm->error),
                     "parameter: expected 0 or 1 arguments, got %d", nargs);
            return CH_VM_RUNTIME_ERROR;
        }
        ChParameter *param = ch_as_parameter(callee);
        size_t result_slot = arg_base;
        if (tail && vm->frame_count > 0) {
            ChCallFrame *frame = &vm->frames[vm->frame_count - 1];
            close_upvalues(vm, frame_regs(vm, frame));
            result_slot = frame->reg_base;
            vm->frame_count--;
        }

        ChValue result = CH_VOID;
        if (nargs == 0) {
            result = ch_vm_parameter_ref(vm, callee);
        } else {
            ChValue new_value = vm->regs[arg_base + 1];
            if (!ch_is_nil(param->converter)) {
                ChValue converted = CH_VOID;
                ChValue convert_args[1] = {new_value};
                ChVMStatus st = ch_vm_apply(vm, param->converter, convert_args, 1, &converted);
                if (st != CH_VM_OK) {
                    return st;
                }
                new_value = converted;
            }
            if (ch_vm_parameter_set(vm, callee, new_value) != 0) {
                return CH_VM_RUNTIME_ERROR;
            }
            result = CH_VOID;
        }
        vm->regs[result_slot] = result;
        if (tail && vm->frame_count == 0) {
            vm->result = result;
        }
        return CH_VM_OK;
    }

    if (!ch_is_closure(callee)) {
        char *printed = ch_value_to_string(callee, false);
        snprintf(vm->error, sizeof(vm->error), "attempt to call non-procedure: %s",
                 printed ? printed : "#<unknown>");
        free(printed);
        return CH_VM_RUNTIME_ERROR;
    }

    ChClosure *cl = ch_as_closure(callee);
    ChFunction *fn = cl->fn;
    if (ch_profile_enabled() || ch_coverage_enabled()) {
        const char *cname = "<lambda>";
        for (size_t g = 0; g < vm->global_count; g++) {
            if (vm->globals[g].defined && ch_eqv(vm->globals[g].value, callee)) {
                cname = vm->globals[g].name->name;
                break;
            }
        }
        ch_profile_enter(cname);
        ch_coverage_hit("scheme", cname);
        /* Leave is approximate: profile depth tracks nested calls of same name. */
        ch_profile_leave(cname);
    }
    int fixed = fn->arity;
    if (fn->variadic) {
        if (nargs < fixed) {
            char buf[256];
            snprintf(buf, sizeof(buf), "wrong number of arguments: expected at least %d, got %d",
                     fixed, nargs);
            return raise_message_to_slot(vm, buf, arg_base);
        }
    } else if (nargs != fixed) {
        char buf[256];
        snprintf(buf, sizeof(buf), "wrong number of arguments: expected %d, got %d", fixed, nargs);
        return raise_message_to_slot(vm, buf, arg_base);
    }

    size_t new_base;
    if (tail && vm->frame_count > 0) {
        ChCallFrame *cur = &vm->frames[vm->frame_count - 1];
        close_upvalues(vm, frame_regs(vm, cur));
        new_base = cur->reg_base;
        if (arg_base != new_base) {
            vm->regs[new_base] = callee;
            for (int i = 0; i < nargs; i++) {
                vm->regs[new_base + 1 + (size_t)i] = vm->regs[arg_base + 1 + (size_t)i];
            }
        }
        vm->frame_count--;
    } else {
        new_base = arg_base;
    }

    ChValue rest = CH_NIL;
    if (fn->variadic) {
        size_t roots = push_gc_roots(vm);
        rest = build_rest_list(vm, &vm->regs[new_base + 1], fixed, nargs);
        pop_gc_roots_n(vm, roots);
    }

    ChVMStatus st = push_frame(vm, cl, new_base);
    if (st != CH_VM_OK) {
        return st;
    }
    ChValue *regs = &vm->regs[new_base];
    for (int i = 0; i < fixed; i++) {
        regs[i] = vm->regs[new_base + 1 + (size_t)i];
    }
    if (fn->variadic) {
        regs[fixed] = rest;
    }
    for (uint8_t i = (uint8_t)(fixed + (fn->variadic ? 1 : 0)); i < fn->num_regs; i++) {
        regs[i] = CH_UNDEFINED;
    }
    return CH_VM_OK;
}

ChVMStatus ch_vm_run_fiber_resume(ChVM *vm, size_t target_frames) {
    return run_until(vm, target_frames);
}

ChVMStatus ch_vm_apply(ChVM *vm, ChValue proc, ChValue *args, int nargs, ChValue *out) {
    size_t saved_frames = vm->frame_count;
    size_t base = vm->reg_top;
    if (base + 1 + (size_t)nargs > CH_VM_MAX_REGS) {
        return CH_VM_STACK_OVERFLOW;
    }
    vm->regs[base] = proc;
    for (int i = 0; i < nargs; i++) {
        vm->regs[base + 1 + (size_t)i] = args[i];
    }
    vm->reg_top = base + 1 + (size_t)nargs;

    ChVMStatus st = call_value(vm, proc, base, nargs, false);
    for (;;) {
        if (st == CH_VM_FIBER_PARKED) {
            vm->reg_top = compute_reg_top(vm);
            return CH_VM_FIBER_PARKED;
        }
        if (st == CH_VM_CONTINUATION_INVOKED) {
            if (vm->frame_count < saved_frames) {
                /* Escaped past this apply. */
                vm->reg_top = compute_reg_top(vm);
                return CH_VM_CONTINUATION_INVOKED;
            }
            if (vm->frame_count == saved_frames) {
                /* Landed at our barrier; invoke_continuation set vm->result. */
                *out = vm->result;
                vm->reg_top = compute_reg_top(vm);
                return CH_VM_OK;
            }
            /* Re-entered above us — keep running until we settle. */
            st = run_until(vm, saved_frames);
            continue;
        }
        if (st != CH_VM_OK) {
            vm->reg_top = compute_reg_top(vm);
            return st;
        }
        if (vm->frame_count > saved_frames) {
            st = run_until(vm, saved_frames);
            continue;
        }
        break;
    }
    *out = vm->regs[base];
    vm->reg_top = compute_reg_top(vm);
    return CH_VM_OK;
}

ChVMStatus ch_vm_eval_function(ChVM *vm, ChFunction *fn, ChValue *out) {
    /* Save caller execution state so nested (eval ...) does not wipe frames. */
    size_t saved_frames = vm->frame_count;
    size_t saved_reg_top = vm->reg_top;
    size_t saved_winds = vm->wind_count;
    size_t saved_handlers = vm->handler_count;
    size_t saved_params = vm->parameter_count;
    ChValue saved_result = vm->result;
    bool saved_cont = vm->continuation_invoked;

    ChValue fn_v = ch_make_pointer(&fn->header);
    ChValue cl_v = CH_FALSE;
    ch_gc_push(&vm->gc, &fn_v);
    ch_gc_push(&vm->gc, &cl_v);
    cl_v = ch_gc_make_closure(&vm->gc, ch_as_function(fn_v), NULL);

    /* Run the thunk in a fresh frame window above the caller's registers. */
    size_t base = saved_reg_top;
    if (base + 1 > CH_VM_MAX_REGS) {
        ch_gc_pop_n(&vm->gc, 2);
        return CH_VM_STACK_OVERFLOW;
    }
    vm->regs[base] = cl_v;
    vm->reg_top = base + 1;
    vm->continuation_invoked = false;

    ChVMStatus st = push_frame(vm, ch_as_closure(cl_v), base);
    if (st != CH_VM_OK) {
        ch_gc_pop_n(&vm->gc, 2);
        vm->frame_count = saved_frames;
        vm->reg_top = saved_reg_top;
        vm->continuation_invoked = saved_cont;
        return st;
    }
    st = run_until(vm, saved_frames);
    if (st == CH_VM_CONTINUATION_INVOKED) {
        if (vm->frame_count < saved_frames) {
            /* Escaped past this eval (e.g. guard call/cc). Do not restore. */
            ch_gc_pop_n(&vm->gc, 2);
            return CH_VM_CONTINUATION_INVOKED;
        }
        if (vm->frame_count == saved_frames) {
            /* Continuation delivered a value at our barrier. */
            *out = vm->result;
            st = CH_VM_OK;
        } else {
            ch_gc_pop_n(&vm->gc, 2);
            return st;
        }
    } else if (st == CH_VM_OK) {
        /* Result: either vm->result (top-level-style return) or regs[base]. */
        if (saved_frames == 0) {
            *out = vm->result;
        } else {
            *out = vm->regs[base];
        }
    }

    vm->frame_count = saved_frames;
    vm->reg_top = saved_reg_top;
    vm->wind_count = saved_winds;
    vm->handler_count = saved_handlers;
    vm->parameter_count = saved_params;
    vm->result = saved_result;
    vm->continuation_invoked = saved_cont;
    ch_gc_pop_n(&vm->gc, 2);
    return st;
}

ChVMStatus ch_vm_call_closure(ChVM *vm, ChValue closure, ChValue *args, int nargs, ChValue *out) {
    if (!ch_is_procedure(closure)) {
        return runtime_error(vm, "not a procedure");
    }
    size_t base = 0;
    vm->regs[base] = closure;
    for (int i = 0; i < nargs; i++) {
        vm->regs[base + 1 + (size_t)i] = args[i];
    }
    vm->reg_top = base + 1 + (size_t)nargs;
    vm->frame_count = 0;
    vm->wind_count = 0;
    vm->handler_count = 0;
    vm->parameter_count = 0;

    ChVMStatus st = call_value(vm, closure, base, nargs, false);
    if (st == CH_VM_CONTINUATION_INVOKED) {
        st = resume_after_continuation(vm, st);
        if (st == CH_VM_OK) {
            *out = vm->result;
        }
        return st;
    }
    if (st != CH_VM_OK) {
        return st;
    }
    if (ch_is_native(closure) || ch_is_continuation(closure)) {
        *out = vm->regs[base];
        return CH_VM_OK;
    }
    st = resume_after_continuation(vm, run_until(vm, 0));
    if (st == CH_VM_OK) {
        *out = vm->result;
    }
    return st;
}

int ch_vm_syntax_property_set(ChVM *vm, const char *id, const char *key, ChValue val) {
    char composite[512];
    if (snprintf(composite, sizeof(composite), "%s\x1f%s", id, key) >= (int)sizeof(composite)) {
        return -1;
    }
    for (size_t i = 0; i < vm->syntax_prop_count; i++) {
        if (strcmp(vm->syntax_props[i].key, composite) == 0) {
            vm->syntax_props[i].value = val;
            return 0;
        }
    }
    if (vm->syntax_prop_count >= CH_VM_MAX_SYNTAX_PROPS) {
        return -1;
    }
    char *owned = strdup(composite);
    if (!owned) {
        return -1;
    }
    vm->syntax_props[vm->syntax_prop_count].key = owned;
    vm->syntax_props[vm->syntax_prop_count].value = val;
    vm->syntax_prop_count++;
    return 0;
}

ChValue ch_vm_syntax_property_get(ChVM *vm, const char *id, const char *key) {
    char composite[512];
    if (snprintf(composite, sizeof(composite), "%s\x1f%s", id, key) >= (int)sizeof(composite)) {
        return CH_FALSE;
    }
    for (size_t i = 0; i < vm->syntax_prop_count; i++) {
        if (strcmp(vm->syntax_props[i].key, composite) == 0) {
            return vm->syntax_props[i].value;
        }
    }
    return CH_FALSE;
}
