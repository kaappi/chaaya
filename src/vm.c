#include "chaaya/vm.h"

#include "chaaya/features.h"
#include "chaaya/library.h"
#include "chaaya/opcode.h"
#include "chaaya/prim.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void ch_vm_init(ChVM *vm) {
    memset(vm, 0, sizeof(*vm));
    ch_gc_init(&vm->gc);
    vm->result = CH_VOID;
    vm->libraries = (ChLibraryRegistry *)calloc(1, sizeof(ChLibraryRegistry));
    if (vm->libraries) {
        ch_library_registry_init(vm->libraries);
    }
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
    ch_register_math_primitives(vm);
    ch_register_port_primitives(vm);
    ch_register_control_primitives(vm);
    ch_register_record_primitives(vm);
    ch_register_lazy_primitives(vm);
    ch_register_features_primitives(vm);
    if (vm->libraries) {
        (void)ch_register_builtin_libraries(vm);
    }
}

const char *ch_vm_error(const ChVM *vm) {
    return vm->error;
}

static ChVMStatus runtime_error(ChVM *vm, const char *msg) {
    snprintf(vm->error, sizeof(vm->error), "%s", msg);
    return CH_VM_RUNTIME_ERROR;
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

static ChValue frame_closure_roots[CH_VM_MAX_FRAMES];

static size_t gc_root_span(ChVM *vm) {
    size_t lib_exports = 0;
    if (vm->libraries) {
        for (size_t i = 0; i < vm->libraries->count; i++) {
            lib_exports += vm->libraries->libs[i]->export_count;
        }
    }
    return vm->reg_top + vm->global_count + vm->wind_count * 2 + vm->handler_count +
           vm->macro_count + vm->frame_count + lib_exports;
}

static void push_gc_roots(ChVM *vm) {
    for (size_t i = 0; i < vm->reg_top; i++) {
        ch_gc_push(&vm->gc, &vm->regs[i]);
    }
    for (size_t i = 0; i < vm->global_count; i++) {
        ch_gc_push(&vm->gc, &vm->globals[i].value);
    }
    for (size_t i = 0; i < vm->wind_count; i++) {
        ch_gc_push(&vm->gc, &vm->wind_stack[i].before);
        ch_gc_push(&vm->gc, &vm->wind_stack[i].after);
    }
    for (size_t i = 0; i < vm->handler_count; i++) {
        ch_gc_push(&vm->gc, &vm->handler_stack[i].handler);
    }
    for (size_t i = 0; i < vm->macro_count; i++) {
        ch_gc_push(&vm->gc, &vm->macros[i].transformer);
    }
    for (size_t i = 0; i < vm->frame_count; i++) {
        if (vm->frames[i].closure) {
            frame_closure_roots[i] = ch_make_pointer(&vm->frames[i].closure->header);
        } else {
            frame_closure_roots[i] = CH_NIL;
        }
        ch_gc_push(&vm->gc, &frame_closure_roots[i]);
    }
    (void)ch_library_push_gc_roots(vm);
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
        return run_until(vm, 0);
    }
    return st;
}

ChValue ch_vm_capture_continuation(ChVM *vm, size_t result_slot) {
    size_t max_reg = vm->reg_top;
    if (result_slot + 1 > max_reg) {
        max_reg = result_slot + 1;
    }

    push_gc_roots(vm);
    size_t roots = gc_root_span(vm);
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
        cont->wind_count > CH_VM_MAX_WINDS || cont->handler_count > CH_VM_MAX_HANDLERS) {
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

    /* Re-sync from snapshot in case before/after thunks mutated the wind stack. */
    memcpy(vm->wind_stack, cont->winds, cont->wind_count * sizeof(ChWindRecord));
    vm->wind_count = cont->wind_count;

    vm->open_upvalues = NULL;
    for (size_t i = 0; i < cont->open_uv_count; i++) {
        ChUpvalue *uv = cont->open_uvs[i].uv;
        size_t idx = cont->open_uvs[i].reg_index;
        if (idx >= vm->reg_top) {
            continue;
        }
        uv->location = &vm->regs[idx];
        uv->is_closed = false;
        uv->next = vm->open_upvalues;
        vm->open_upvalues = uv;
    }

    vm->regs[cont->result_slot] = value;
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
                ChLibEnv *env = vm->active_lib_env;
                if (!env || li >= env->count || !env->bindings[li].defined) {
                    snprintf(vm->error, sizeof(vm->error), "unbound variable: %s",
                             env && li < env->count ? env->bindings[li].name->name : "?");
                    return CH_VM_RUNTIME_ERROR;
                }
                regs[dst] = env->bindings[li].value;
            } else {
                if (!vm->globals[idx].defined) {
                    snprintf(vm->error, sizeof(vm->error), "unbound variable: %s",
                             vm->globals[idx].name->name);
                    return CH_VM_RUNTIME_ERROR;
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
                ChLibEnv *env = vm->active_lib_env;
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
                ChLibEnv *env = vm->active_lib_env;
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
            break;
        }
        case CH_OP_CONS: {
            uint8_t dst = read_u8(frame);
            uint8_t car = read_u8(frame);
            uint8_t cdr = read_u8(frame);
            push_gc_roots(vm);
            size_t roots = gc_root_span(vm);
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
            push_gc_roots(vm);
            size_t roots = gc_root_span(vm);
            ChValue fn_root = fn_v;
            ch_gc_push(&vm->gc, &fn_root);
            regs[dst] = ch_gc_make_closure(&vm->gc, ch_as_function(fn_root), uvs);
            ch_gc_pop_n(&vm->gc, roots + 1);
            break;
        }
        case CH_OP_CALL:
        case CH_OP_TAIL_CALL: {
            uint8_t base = read_u8(frame);
            uint8_t nargs = read_u8(frame);
            ChVMStatus st =
                call_value(vm, regs[base], frame->reg_base + base, nargs, op == CH_OP_TAIL_CALL);
            if (st == CH_VM_CONTINUATION_INVOKED) {
                /* Nested apply must stop mid-wind; outermost run_until(0) resumes. */
                if (target_frames > 0) {
                    return CH_VM_CONTINUATION_INVOKED;
                }
                continue;
            }
            if (st != CH_VM_OK) {
                return st;
            }
            /* Discard extra values except in tail context (for call-with-values). */
            if (op == CH_OP_CALL) {
                regs[base] = ch_coerce_single(regs[base]);
            }
            break;
        }
        case CH_OP_RETURN: {
            uint8_t src = read_u8(frame);
            ChValue result = regs[src];
            close_upvalues(vm, regs);
            if (vm->frame_count == 1) {
                vm->result = ch_coerce_single(result);
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
            return runtime_error(vm, "invalid opcode");
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

static ChVMStatus call_value(ChVM *vm, ChValue callee, size_t arg_base, int nargs, bool tail) {
    if (ch_is_continuation(callee)) {
        if (nargs != 1) {
            snprintf(vm->error, sizeof(vm->error), "continuation: expected 1 argument, got %d", nargs);
            return CH_VM_RUNTIME_ERROR;
        }
        return ch_vm_invoke_continuation(vm, ch_as_continuation(callee), vm->regs[arg_base + 1]);
    }

    if (ch_is_native(callee)) {
        ChNative *n = ch_as_native(callee);
        if (n->arity >= 0 && nargs != n->arity) {
            snprintf(vm->error, sizeof(vm->error), "%s: expected %d args, got %d", n->name, n->arity,
                     nargs);
            return CH_VM_RUNTIME_ERROR;
        }
        if (n->arity < 0 && nargs < n->min_arity) {
            snprintf(vm->error, sizeof(vm->error), "%s: expected at least %d args, got %d", n->name,
                     n->min_arity, nargs);
            return CH_VM_RUNTIME_ERROR;
        }
        ChValue *args = &vm->regs[arg_base + 1];
        size_t result_slot = arg_base;
        if (tail && vm->frame_count > 0) {
            /* Drop current frame first so call/cc captures the caller (R7RS tail). */
            ChCallFrame *frame = &vm->frames[vm->frame_count - 1];
            close_upvalues(vm, frame_regs(vm, frame));
            result_slot = frame->reg_base;
            vm->frame_count--;
        }
        vm->native_result_slot = result_slot;
        vm->continuation_invoked = false;
        size_t roots = gc_root_span(vm);
        push_gc_roots(vm);
        ChValue result = n->fn(vm, args, nargs);
        pop_gc_roots_n(vm, roots);
        if (vm->continuation_invoked) {
            vm->continuation_invoked = false;
            return CH_VM_CONTINUATION_INVOKED;
        }
        if (vm->error[0] != '\0' && result == CH_UNDEFINED) {
            return CH_VM_RUNTIME_ERROR;
        }
        vm->regs[result_slot] = result;
        if (tail && vm->frame_count == 0) {
            vm->result = result;
        }
        return CH_VM_OK;
    }

    if (!ch_is_closure(callee)) {
        return runtime_error(vm, "attempt to call non-procedure");
    }

    ChClosure *cl = ch_as_closure(callee);
    ChFunction *fn = cl->fn;
    int fixed = fn->arity;
    if (fn->variadic) {
        if (nargs < fixed) {
            snprintf(vm->error, sizeof(vm->error),
                     "wrong number of arguments: expected at least %d, got %d", fixed, nargs);
            return CH_VM_RUNTIME_ERROR;
        }
    } else if (nargs != fixed) {
        snprintf(vm->error, sizeof(vm->error), "wrong number of arguments: expected %d, got %d", fixed,
                 nargs);
        return CH_VM_RUNTIME_ERROR;
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
        size_t roots = gc_root_span(vm);
        push_gc_roots(vm);
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
    if (st != CH_VM_OK) {
        return st;
    }
    if (vm->frame_count > saved_frames) {
        st = run_until(vm, saved_frames);
        if (st != CH_VM_OK) {
            return st;
        }
    }
    *out = vm->regs[base];
    return CH_VM_OK;
}

ChVMStatus ch_vm_eval_function(ChVM *vm, ChFunction *fn, ChValue *out) {
    ChValue fn_v = ch_make_pointer(&fn->header);
    ch_gc_push(&vm->gc, &fn_v);
    for (size_t i = 0; i < vm->global_count; i++) {
        ch_gc_push(&vm->gc, &vm->globals[i].value);
    }
    ChValue cl_v = ch_gc_make_closure(&vm->gc, ch_as_function(fn_v), NULL);
    /* Install closure in regs before dropping roots so a GC cannot collect it. */
    vm->reg_top = 1;
    vm->frame_count = 0;
    vm->wind_count = 0;
    vm->handler_count = 0;
    vm->continuation_invoked = false;
    vm->regs[0] = cl_v;
    ch_gc_pop_n(&vm->gc, 1 + vm->global_count);
    ChVMStatus st = push_frame(vm, ch_as_closure(vm->regs[0]), 0);
    if (st != CH_VM_OK) {
        return st;
    }
    st = resume_after_continuation(vm, run_until(vm, 0));
    if (st == CH_VM_OK) {
        *out = vm->result;
    }
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

    ChVMStatus st = call_value(vm, closure, base, nargs, false);
    if (st == CH_VM_CONTINUATION_INVOKED) {
        st = run_until(vm, 0);
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
