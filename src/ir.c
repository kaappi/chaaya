#include "chaaya/ir.h"

#include <stdlib.h>

ChIrNode *ch_ir_new_node(ChIrKind kind) {
    ChIrNode *node = (ChIrNode *)calloc(1, sizeof(*node));
    if (!node) {
        abort();
    }
    node->kind = kind;
    node->tail_position = false;
    node->is_constant = false;
    node->constant_value = CH_UNDEFINED;
    return node;
}

static void free_node_array(ChIrNode **items, size_t count) {
    if (!items) {
        return;
    }
    for (size_t i = 0; i < count; i++) {
        ch_ir_free(items[i]);
    }
    free(items);
}

void ch_ir_free(ChIrNode *node) {
    if (!node) {
        return;
    }
    switch (node->kind) {
    case CH_IR_IF:
        ch_ir_free(node->as.if_expr.test);
        ch_ir_free(node->as.if_expr.consequent);
        ch_ir_free(node->as.if_expr.alternate);
        break;
    case CH_IR_LAMBDA:
        free_node_array(node->as.lambda.body, node->as.lambda.body_count);
        break;
    case CH_IR_SEQ:
        free_node_array(node->as.seq.items, node->as.seq.count);
        break;
    case CH_IR_CALL:
        ch_ir_free(node->as.call.callee);
        free_node_array(node->as.call.args, node->as.call.arg_count);
        break;
    case CH_IR_SET:
        ch_ir_free(node->as.set_expr.value);
        break;
    case CH_IR_DEFINE:
        ch_ir_free(node->as.define_expr.value);
        break;
    case CH_IR_AND:
        free_node_array(node->as.and_expr.items, node->as.and_expr.count);
        break;
    case CH_IR_OR:
        free_node_array(node->as.or_expr.items, node->as.or_expr.count);
        break;
    case CH_IR_PRIM_CALL:
        free_node_array(node->as.prim_call.args, node->as.prim_call.arg_count);
        break;
    case CH_IR_VOID:
    case CH_IR_LITERAL:
    case CH_IR_QUOTE:
    case CH_IR_VAR:
    case CH_IR_DEFINE_SYNTAX:
    case CH_IR_RAW:
        break;
    }
    free(node);
}
