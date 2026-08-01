#include "chaaya/ir.h"

#include <stdio.h>
#include <stdlib.h>

static const char *prim_name(ChIrPrim prim) {
    switch (prim) {
    case CH_IR_PRIM_ADD:
        return "+";
    case CH_IR_PRIM_SUB:
        return "-";
    case CH_IR_PRIM_MUL:
        return "*";
    case CH_IR_PRIM_LT:
        return "<";
    case CH_IR_PRIM_GT:
        return ">";
    case CH_IR_PRIM_NUM_EQ:
        return "=";
    case CH_IR_PRIM_LE:
        return "<=";
    case CH_IR_PRIM_GE:
        return ">=";
    case CH_IR_PRIM_NOT:
        return "not";
    case CH_IR_PRIM_COUNT:
        return NULL;
    }
    return NULL;
}

static ChValue build_list(ChGC *gc, ChValue *items, size_t count) {
    ChValue list = CH_NIL;
    ch_gc_push(gc, &list);
    for (size_t i = count; i > 0; i--) {
        list = ch_gc_cons(gc, items[i - 1], list);
    }
    ch_gc_pop(gc);
    return list;
}

static ChValue build_form(ChGC *gc, ChValue head, ChValue *args, size_t nargs) {
    ChValue list = build_list(gc, args, nargs);
    ch_gc_push(gc, &head);
    ch_gc_push(gc, &list);
    ChValue form = ch_gc_cons(gc, head, list);
    ch_gc_pop_n(gc, 2);
    return form;
}

static ChCompileStatus ir_node_to_expr(ChCompiler *c, ChIrNode *node, ChValue *out_expr);

static ChCompileStatus convert_node_array(ChCompiler *c, ChIrNode **nodes, size_t count,
                                          ChValue **out_values) {
    ChGC *gc = &c->vm->gc;
    ChValue *values = NULL;
    if (count > 0) {
        values = (ChValue *)calloc(count, sizeof(*values));
        if (!values) {
            abort();
        }
    }
    for (size_t i = 0; i < count; i++) {
        values[i] = CH_NIL;
        ch_gc_push(gc, &values[i]);
    }
    for (size_t i = 0; i < count; i++) {
        if (ir_node_to_expr(c, nodes[i], &values[i]) != CH_COMPILE_OK) {
            ch_gc_pop_n(gc, count);
            free(values);
            return CH_COMPILE_ERROR;
        }
    }
    *out_values = values;
    return CH_COMPILE_OK;
}

static void pop_converted_nodes(ChGC *gc, ChValue *values, size_t count) {
    ch_gc_pop_n(gc, count);
    free(values);
}

static ChCompileStatus emit_keyword_form(ChCompiler *c, const char *keyword, ChValue *args,
                                         size_t nargs, ChValue *out_expr) {
    ChGC *gc = &c->vm->gc;
    ChValue head = ch_gc_intern_symbol_cstr(gc, keyword);
    ch_gc_push(gc, &head);
    *out_expr = build_form(gc, head, args, nargs);
    ch_gc_pop(gc);
    return CH_COMPILE_OK;
}

static ChCompileStatus ir_node_to_expr(ChCompiler *c, ChIrNode *node, ChValue *out_expr) {
    if (!node) {
        snprintf(c->error, sizeof(c->error), "ir emit: null node");
        return CH_COMPILE_ERROR;
    }

    ChGC *gc = &c->vm->gc;
    switch (node->kind) {
    case CH_IR_VOID:
        *out_expr = CH_VOID;
        return CH_COMPILE_OK;
    case CH_IR_LITERAL:
        *out_expr = node->as.literal;
        return CH_COMPILE_OK;
    case CH_IR_QUOTE: {
        ChValue args[1];
        args[0] = node->as.quoted;
        ch_gc_push(gc, &args[0]);
        ChCompileStatus st = emit_keyword_form(c, "quote", args, 1, out_expr);
        ch_gc_pop(gc);
        return st;
    }
    case CH_IR_VAR:
        *out_expr = ch_make_pointer(&node->as.var->header);
        return CH_COMPILE_OK;
    case CH_IR_IF: {
        ChValue test = CH_NIL;
        ChValue consequent = CH_NIL;
        ChValue alternate = CH_NIL;
        ch_gc_push(gc, &test);
        ch_gc_push(gc, &consequent);
        ch_gc_push(gc, &alternate);
        if (ir_node_to_expr(c, node->as.if_expr.test, &test) != CH_COMPILE_OK ||
            ir_node_to_expr(c, node->as.if_expr.consequent, &consequent) != CH_COMPILE_OK ||
            (node->as.if_expr.has_alternate &&
             ir_node_to_expr(c, node->as.if_expr.alternate, &alternate) != CH_COMPILE_OK)) {
            ch_gc_pop_n(gc, 3);
            return CH_COMPILE_ERROR;
        }
        ChValue args[3];
        args[0] = test;
        args[1] = consequent;
        size_t nargs = 2;
        if (node->as.if_expr.has_alternate) {
            args[2] = alternate;
            nargs = 3;
        }
        ChCompileStatus st = emit_keyword_form(c, "if", args, nargs, out_expr);
        ch_gc_pop_n(gc, 3);
        return st;
    }
    case CH_IR_LAMBDA: {
        ChValue params = node->as.lambda.params;
        ch_gc_push(gc, &params);
        ChValue *body_values = NULL;
        if (convert_node_array(c, node->as.lambda.body, node->as.lambda.body_count, &body_values) !=
            CH_COMPILE_OK) {
            ch_gc_pop(gc);
            return CH_COMPILE_ERROR;
        }
        size_t nargs = node->as.lambda.body_count + 1;
        ChValue *args = (ChValue *)calloc(nargs, sizeof(*args));
        if (!args) {
            abort();
        }
        args[0] = params;
        for (size_t i = 0; i < node->as.lambda.body_count; i++) {
            args[i + 1] = body_values[i];
        }
        ChCompileStatus st = emit_keyword_form(c, "lambda", args, nargs, out_expr);
        free(args);
        pop_converted_nodes(gc, body_values, node->as.lambda.body_count);
        ch_gc_pop(gc);
        return st;
    }
    case CH_IR_SEQ: {
        if (node->as.seq.count == 0) {
            *out_expr = CH_VOID;
            return CH_COMPILE_OK;
        }
        ChValue *values = NULL;
        if (convert_node_array(c, node->as.seq.items, node->as.seq.count, &values) != CH_COMPILE_OK) {
            return CH_COMPILE_ERROR;
        }
        ChCompileStatus st = emit_keyword_form(c, "begin", values, node->as.seq.count, out_expr);
        pop_converted_nodes(gc, values, node->as.seq.count);
        return st;
    }
    case CH_IR_CALL: {
        ChValue callee = CH_NIL;
        ch_gc_push(gc, &callee);
        if (ir_node_to_expr(c, node->as.call.callee, &callee) != CH_COMPILE_OK) {
            ch_gc_pop(gc);
            return CH_COMPILE_ERROR;
        }
        ChValue *arg_values = NULL;
        if (convert_node_array(c, node->as.call.args, node->as.call.arg_count, &arg_values) !=
            CH_COMPILE_OK) {
            ch_gc_pop(gc);
            return CH_COMPILE_ERROR;
        }
        ChValue arg_list = build_list(gc, arg_values, node->as.call.arg_count);
        ch_gc_push(gc, &arg_list);
        *out_expr = ch_gc_cons(gc, callee, arg_list);
        ch_gc_pop(gc);
        pop_converted_nodes(gc, arg_values, node->as.call.arg_count);
        ch_gc_pop(gc);
        return CH_COMPILE_OK;
    }
    case CH_IR_SET: {
        ChValue name = ch_make_pointer(&node->as.set_expr.name->header);
        ChValue value = CH_NIL;
        ch_gc_push(gc, &name);
        ch_gc_push(gc, &value);
        if (ir_node_to_expr(c, node->as.set_expr.value, &value) != CH_COMPILE_OK) {
            ch_gc_pop_n(gc, 2);
            return CH_COMPILE_ERROR;
        }
        ChValue args[2] = {name, value};
        ChCompileStatus st = emit_keyword_form(c, "set!", args, 2, out_expr);
        ch_gc_pop_n(gc, 2);
        return st;
    }
    case CH_IR_DEFINE: {
        ChValue value = CH_NIL;
        ChValue target = node->as.define_expr.target;
        ch_gc_push(gc, &target);
        ch_gc_push(gc, &value);
        if (ir_node_to_expr(c, node->as.define_expr.value, &value) != CH_COMPILE_OK) {
            ch_gc_pop_n(gc, 2);
            return CH_COMPILE_ERROR;
        }
        ChValue args[2] = {target, value};
        ChCompileStatus st = emit_keyword_form(c, "define", args, 2, out_expr);
        ch_gc_pop_n(gc, 2);
        return st;
    }
    case CH_IR_DEFINE_SYNTAX:
        *out_expr = CH_VOID;
        return CH_COMPILE_OK;
    case CH_IR_AND: {
        ChValue *values = NULL;
        if (convert_node_array(c, node->as.and_expr.items, node->as.and_expr.count, &values) !=
            CH_COMPILE_OK) {
            return CH_COMPILE_ERROR;
        }
        ChCompileStatus st = emit_keyword_form(c, "and", values, node->as.and_expr.count, out_expr);
        pop_converted_nodes(gc, values, node->as.and_expr.count);
        return st;
    }
    case CH_IR_OR: {
        ChValue *values = NULL;
        if (convert_node_array(c, node->as.or_expr.items, node->as.or_expr.count, &values) !=
            CH_COMPILE_OK) {
            return CH_COMPILE_ERROR;
        }
        ChCompileStatus st = emit_keyword_form(c, "or", values, node->as.or_expr.count, out_expr);
        pop_converted_nodes(gc, values, node->as.or_expr.count);
        return st;
    }
    case CH_IR_PRIM_CALL: {
        const char *name = prim_name(node->as.prim_call.prim);
        ChValue callee = CH_NIL;
        if (node->as.prim_call.symbol) {
            callee = ch_make_pointer(&node->as.prim_call.symbol->header);
        } else {
            if (!name) {
                snprintf(c->error, sizeof(c->error), "ir emit: unknown primitive");
                return CH_COMPILE_ERROR;
            }
            callee = ch_gc_intern_symbol_cstr(gc, name);
        }
        ch_gc_push(gc, &callee);
        ChValue *arg_values = NULL;
        if (convert_node_array(c, node->as.prim_call.args, node->as.prim_call.arg_count,
                               &arg_values) != CH_COMPILE_OK) {
            ch_gc_pop(gc);
            return CH_COMPILE_ERROR;
        }
        ChValue arg_list = build_list(gc, arg_values, node->as.prim_call.arg_count);
        ch_gc_push(gc, &arg_list);
        *out_expr = ch_gc_cons(gc, callee, arg_list);
        ch_gc_pop(gc);
        pop_converted_nodes(gc, arg_values, node->as.prim_call.arg_count);
        ch_gc_pop(gc);
        return CH_COMPILE_OK;
    }
    case CH_IR_RAW:
        *out_expr = node->as.raw.expr;
        return CH_COMPILE_OK;
    }

    snprintf(c->error, sizeof(c->error), "ir emit: unknown node kind");
    return CH_COMPILE_ERROR;
}

ChCompileStatus ch_ir_emit(ChCompiler *c, ChIrNode *root, ChIrLegacyEmitFn emit_fn, void *emit_ctx,
                           uint8_t dst, bool tail) {
    if (!emit_fn) {
        snprintf(c->error, sizeof(c->error), "ir emit: missing emit callback");
        return CH_COMPILE_ERROR;
    }
    ChValue expr = CH_VOID;
    if (ir_node_to_expr(c, root, &expr) != CH_COMPILE_OK) {
        return CH_COMPILE_ERROR;
    }
    ch_gc_push(&c->vm->gc, &expr);
    ChCompileStatus st = emit_fn(emit_ctx, expr, dst, tail);
    ch_gc_pop(&c->vm->gc);
    return st;
}
