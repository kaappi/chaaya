#include "chaaya/ir.h"

static bool is_truthy(ChValue v) {
    return v != CH_FALSE;
}

static void set_constant(ChIrNode *node, bool is_constant, ChValue value) {
    node->is_constant = is_constant;
    node->constant_value = is_constant ? value : CH_UNDEFINED;
}

static void analyze_node(ChIrNode *node, bool tail_position) {
    if (!node) {
        return;
    }

    node->tail_position = tail_position;
    switch (node->kind) {
    case CH_IR_VOID:
        set_constant(node, true, CH_VOID);
        return;
    case CH_IR_LITERAL:
        set_constant(node, true, node->as.literal);
        return;
    case CH_IR_QUOTE:
        set_constant(node, true, node->as.quoted);
        return;
    case CH_IR_VAR:
        set_constant(node, false, CH_UNDEFINED);
        return;
    case CH_IR_IF: {
        analyze_node(node->as.if_expr.test, false);
        analyze_node(node->as.if_expr.consequent, tail_position);
        if (node->as.if_expr.has_alternate) {
            analyze_node(node->as.if_expr.alternate, tail_position);
        }
        if (node->as.if_expr.test && node->as.if_expr.test->is_constant) {
            if (is_truthy(node->as.if_expr.test->constant_value)) {
                if (node->as.if_expr.consequent && node->as.if_expr.consequent->is_constant) {
                    set_constant(node, true, node->as.if_expr.consequent->constant_value);
                    return;
                }
            } else if (node->as.if_expr.has_alternate) {
                if (node->as.if_expr.alternate && node->as.if_expr.alternate->is_constant) {
                    set_constant(node, true, node->as.if_expr.alternate->constant_value);
                    return;
                }
            } else {
                set_constant(node, true, CH_FALSE);
                return;
            }
        }
        set_constant(node, false, CH_UNDEFINED);
        return;
    }
    case CH_IR_LAMBDA: {
        for (size_t i = 0; i < node->as.lambda.body_count; i++) {
            bool tail = (i + 1 == node->as.lambda.body_count);
            analyze_node(node->as.lambda.body[i], tail);
        }
        set_constant(node, false, CH_UNDEFINED);
        return;
    }
    case CH_IR_SEQ: {
        if (node->as.seq.count == 0) {
            set_constant(node, true, CH_VOID);
            return;
        }
        bool all_constant = true;
        ChValue last = CH_VOID;
        for (size_t i = 0; i < node->as.seq.count; i++) {
            bool tail = tail_position && (i + 1 == node->as.seq.count);
            analyze_node(node->as.seq.items[i], tail);
            if (!node->as.seq.items[i] || !node->as.seq.items[i]->is_constant) {
                all_constant = false;
            } else {
                last = node->as.seq.items[i]->constant_value;
            }
        }
        set_constant(node, all_constant, all_constant ? last : CH_UNDEFINED);
        return;
    }
    case CH_IR_CALL: {
        analyze_node(node->as.call.callee, false);
        for (size_t i = 0; i < node->as.call.arg_count; i++) {
            analyze_node(node->as.call.args[i], false);
        }
        set_constant(node, false, CH_UNDEFINED);
        return;
    }
    case CH_IR_SET:
        analyze_node(node->as.set_expr.value, false);
        set_constant(node, false, CH_UNDEFINED);
        return;
    case CH_IR_DEFINE:
        analyze_node(node->as.define_expr.value, false);
        set_constant(node, false, CH_UNDEFINED);
        return;
    case CH_IR_DEFINE_SYNTAX:
        set_constant(node, true, CH_VOID);
        return;
    case CH_IR_AND: {
        if (node->as.and_expr.count == 0) {
            set_constant(node, true, CH_TRUE);
            return;
        }
        bool constant = true;
        ChValue result = CH_TRUE;
        for (size_t i = 0; i < node->as.and_expr.count; i++) {
            bool tail = tail_position && (i + 1 == node->as.and_expr.count);
            ChIrNode *child = node->as.and_expr.items[i];
            analyze_node(child, tail);
            if (!child || !child->is_constant) {
                constant = false;
                break;
            }
            result = child->constant_value;
            if (!is_truthy(result)) {
                break;
            }
        }
        set_constant(node, constant, constant ? result : CH_UNDEFINED);
        return;
    }
    case CH_IR_OR: {
        if (node->as.or_expr.count == 0) {
            set_constant(node, true, CH_FALSE);
            return;
        }
        bool constant = true;
        ChValue result = CH_FALSE;
        for (size_t i = 0; i < node->as.or_expr.count; i++) {
            bool tail = tail_position && (i + 1 == node->as.or_expr.count);
            ChIrNode *child = node->as.or_expr.items[i];
            analyze_node(child, tail);
            if (!child || !child->is_constant) {
                constant = false;
                break;
            }
            result = child->constant_value;
            if (is_truthy(result)) {
                break;
            }
        }
        set_constant(node, constant, constant ? result : CH_UNDEFINED);
        return;
    }
    case CH_IR_PRIM_CALL:
        for (size_t i = 0; i < node->as.prim_call.arg_count; i++) {
            analyze_node(node->as.prim_call.args[i], false);
        }
        set_constant(node, false, CH_UNDEFINED);
        return;
    case CH_IR_RAW:
        set_constant(node, false, CH_UNDEFINED);
        return;
    }
}

void ch_ir_analyze(ChIrNode *root) {
    analyze_node(root, true);
}
