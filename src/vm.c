#include "chaaya/vm.h"

#include "chaaya/opcode.h"
#include "chaaya/prim.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void ch_vm_init(ChVM *vm) {
    memset(vm, 0, sizeof(*vm));
    ch_gc_init(&vm->gc);
    vm->reg_top = 0;
    vm->frame_count = 0;
    vm->global_count = 0;
    vm->open_upvalues = NULL;
    vm->result = CH_VOID;
    vm->error[0] = '\0';
}

void ch_vm_deinit(ChVM *vm) {
    ChUpvalue *uv = vm->open_upvalues;
    while (uv) {
        ChUpvalue *next = uv->next;
        free(uv);
        uv = next;
    }
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

static void push_gc_roots(ChVM *vm) {
    for (size_t i = 0; i < vm->reg_top; i++) {
        ch_gc_push(&vm->gc, &vm->regs[i]);
    }
    for (size_t i = 0; i < vm->global_count; i++) {
        ch_gc_push(&vm->gc, &vm->globals[i].value);
    }
}

static void pop_gc_roots(ChVM *vm) {
    ch_gc_pop_n(&vm->gc, vm->reg_top + vm->global_count);
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

static ChVMStatus call_value(ChVM *vm, ChValue callee, size_t arg_base, int nargs, bool tail,
                             ChValue *out_result);

static ChVMStatus run(ChVM *vm) {
    for (;;) {
        if (vm->frame_count == 0) {
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
            if (!vm->globals[idx].defined) {
                snprintf(vm->error, sizeof(vm->error), "unbound variable: %s",
                         vm->globals[idx].name->name);
                return CH_VM_RUNTIME_ERROR;
            }
            regs[dst] = vm->globals[idx].value;
            break;
        }
        case CH_OP_SET_GLOBAL: {
            uint16_t idx = read_u16(frame);
            uint8_t src = read_u8(frame);
            if (!vm->globals[idx].defined) {
                snprintf(vm->error, sizeof(vm->error), "unbound variable: %s",
                         vm->globals[idx].name->name);
                return CH_VM_RUNTIME_ERROR;
            }
            vm->globals[idx].value = regs[src];
            break;
        }
        case CH_OP_DEFINE_GLOBAL: {
            uint16_t idx = read_u16(frame);
            uint8_t src = read_u8(frame);
            vm->globals[idx].value = regs[src];
            vm->globals[idx].defined = true;
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
            regs[dst] = ch_gc_cons(&vm->gc, regs[car], regs[cdr]);
            pop_gc_roots(vm);
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
            ChValue fn_root = fn_v;
            ch_gc_push(&vm->gc, &fn_root);
            regs[dst] = ch_gc_make_closure(&vm->gc, ch_as_function(fn_root), uvs);
            ch_gc_pop_n(&vm->gc, vm->reg_top + vm->global_count + 1);
            break;
        }
        case CH_OP_CALL:
        case CH_OP_TAIL_CALL: {
            uint8_t base = read_u8(frame);
            uint8_t nargs = read_u8(frame);
            ChVMStatus st =
                call_value(vm, regs[base], frame->reg_base + base, nargs, op == CH_OP_TAIL_CALL, NULL);
            if (st != CH_VM_OK) {
                return st;
            }
            break;
        }
        case CH_OP_RETURN: {
            uint8_t src = read_u8(frame);
            ChValue result = regs[src];
            close_upvalues(vm, regs);
            if (vm->frame_count == 1) {
                vm->result = result;
                vm->frame_count = 0;
                return CH_VM_OK;
            }
            size_t ret_slot = frame->reg_base;
            vm->frame_count--;
            vm->regs[ret_slot] = result;
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

static ChVMStatus call_value(ChVM *vm, ChValue callee, size_t arg_base, int nargs, bool tail,
                             ChValue *out_result) {
    (void)out_result;
    if (ch_is_native(callee)) {
        ChNative *n = ch_as_native(callee);
        if (n->arity >= 0) {
            if (nargs != n->arity) {
                snprintf(vm->error, sizeof(vm->error), "%s: expected %d args, got %d", n->name,
                         n->arity, nargs);
                return CH_VM_RUNTIME_ERROR;
            }
        } else if (nargs < n->min_arity) {
            snprintf(vm->error, sizeof(vm->error), "%s: expected at least %d args, got %d", n->name,
                     n->min_arity, nargs);
            return CH_VM_RUNTIME_ERROR;
        }
        ChValue *args = &vm->regs[arg_base + 1];
        push_gc_roots(vm);
        ChValue result = n->fn(vm, args, nargs);
        pop_gc_roots(vm);
        vm->regs[arg_base] = result;
        if (tail && vm->frame_count > 0) {
            /* return to caller immediately */
            ChCallFrame *frame = &vm->frames[vm->frame_count - 1];
            close_upvalues(vm, frame_regs(vm, frame));
            if (vm->frame_count == 1) {
                vm->result = result;
                vm->frame_count = 0;
            } else {
                size_t ret_slot = frame->reg_base;
                vm->frame_count--;
                vm->regs[ret_slot] = result;
            }
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
            snprintf(vm->error, sizeof(vm->error), "wrong number of arguments: expected at least %d, got %d",
                     fixed, nargs);
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
        /* move args down to current frame base */
        new_base = cur->reg_base;
        /* shift: callee at new_base, args after — already at arg_base */
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

    /* Prepare rest argument */
    ChValue rest = CH_NIL;
    if (fn->variadic) {
        push_gc_roots(vm);
        rest = build_rest_list(vm, &vm->regs[new_base + 1], fixed, nargs);
        pop_gc_roots(vm);
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

ChVMStatus ch_vm_eval_function(ChVM *vm, ChFunction *fn, ChValue *out) {
    /* Wrap bare function in a closure with no upvalues */
    ChValue fn_v = ch_make_pointer(&fn->header);
    ch_gc_push(&vm->gc, &fn_v);
    for (size_t i = 0; i < vm->global_count; i++) {
        ch_gc_push(&vm->gc, &vm->globals[i].value);
    }
    ChValue cl_v = ch_gc_make_closure(&vm->gc, ch_as_function(fn_v), NULL);
    ch_gc_pop_n(&vm->gc, 1 + vm->global_count);

    vm->reg_top = 0;
    vm->frame_count = 0;
    vm->regs[0] = cl_v;
    ChVMStatus st = push_frame(vm, ch_as_closure(cl_v), 0);
    if (st != CH_VM_OK) {
        return st;
    }
    st = run(vm);
    if (st == CH_VM_OK) {
        *out = vm->result;
    }
    return st;
}

ChVMStatus ch_vm_call_closure(ChVM *vm, ChValue closure, ChValue *args, int nargs, ChValue *out) {
    if (!ch_is_closure(closure) && !ch_is_native(closure)) {
        return runtime_error(vm, "not a procedure");
    }
    size_t base = 0;
    vm->regs[base] = closure;
    for (int i = 0; i < nargs; i++) {
        vm->regs[base + 1 + (size_t)i] = args[i];
    }
    vm->reg_top = base + 1 + (size_t)nargs;
    vm->frame_count = 0;
    ChVMStatus st = call_value(vm, closure, base, nargs, false, NULL);
    if (st != CH_VM_OK) {
        return st;
    }
    if (ch_is_native(closure)) {
        *out = vm->regs[base];
        return CH_VM_OK;
    }
    st = run(vm);
    if (st == CH_VM_OK) {
        *out = vm->result;
    }
    return st;
}
