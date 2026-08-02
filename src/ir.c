#include "chaaya/ir.h"

#include "chaaya/printer.h"
#include "chaaya/value.h"

#include <stdlib.h>
#include <string.h>

static const char *ir_kind_name(ChIrKind kind) {
    switch (kind) {
    case CH_IR_VOID:
        return "void";
    case CH_IR_LITERAL:
        return "literal";
    case CH_IR_QUOTE:
        return "quote";
    case CH_IR_VAR:
        return "var";
    case CH_IR_IF:
        return "if";
    case CH_IR_LAMBDA:
        return "lambda";
    case CH_IR_SEQ:
        return "seq";
    case CH_IR_CALL:
        return "call";
    case CH_IR_SET:
        return "set!";
    case CH_IR_DEFINE:
        return "define";
    case CH_IR_DEFINE_SYNTAX:
        return "define-syntax";
    case CH_IR_AND:
        return "and";
    case CH_IR_OR:
        return "or";
    case CH_IR_PRIM_CALL:
        return "prim";
    case CH_IR_RAW:
        return "raw";
    }
    return "?";
}

static void print_indent(FILE *out, int indent) {
    for (int i = 0; i < indent; i++) {
        fputc(' ', out);
    }
}

void ch_ir_print(FILE *out, const ChIrNode *node, int indent) {
    if (!out) {
        return;
    }
    if (!node) {
        print_indent(out, indent);
        fputs("(null)\n", out);
        return;
    }
    print_indent(out, indent);
    fprintf(out, "(%s", ir_kind_name(node->kind));
    if (node->tail_position) {
        fputs(" tail", out);
    }
    switch (node->kind) {
    case CH_IR_VOID:
        fputs(")\n", out);
        break;
    case CH_IR_LITERAL:
        fputc(' ', out);
        ch_print_value(out, node->as.literal, false);
        fputs(")\n", out);
        break;
    case CH_IR_QUOTE:
        fputc(' ', out);
        ch_print_value(out, node->as.quoted, false);
        fputs(")\n", out);
        break;
    case CH_IR_VAR:
        fprintf(out, " %s)\n", node->as.var ? node->as.var->name : "?");
        break;
    case CH_IR_IF:
        fputs("\n", out);
        ch_ir_print(out, node->as.if_expr.test, indent + 2);
        ch_ir_print(out, node->as.if_expr.consequent, indent + 2);
        if (node->as.if_expr.has_alternate) {
            ch_ir_print(out, node->as.if_expr.alternate, indent + 2);
        }
        print_indent(out, indent);
        fputs(")\n", out);
        break;
    case CH_IR_LAMBDA:
        fputc(' ', out);
        ch_print_value(out, node->as.lambda.params, false);
        fputs("\n", out);
        for (size_t i = 0; i < node->as.lambda.body_count; i++) {
            ch_ir_print(out, node->as.lambda.body[i], indent + 2);
        }
        print_indent(out, indent);
        fputs(")\n", out);
        break;
    case CH_IR_SEQ:
    case CH_IR_AND:
    case CH_IR_OR: {
        const ChIrNodeArray *arr =
            node->kind == CH_IR_SEQ     ? &node->as.seq
            : node->kind == CH_IR_AND   ? &node->as.and_expr
                                        : &node->as.or_expr;
        fputs("\n", out);
        for (size_t i = 0; i < arr->count; i++) {
            ch_ir_print(out, arr->items[i], indent + 2);
        }
        print_indent(out, indent);
        fputs(")\n", out);
        break;
    }
    case CH_IR_CALL:
        fputs("\n", out);
        ch_ir_print(out, node->as.call.callee, indent + 2);
        for (size_t i = 0; i < node->as.call.arg_count; i++) {
            ch_ir_print(out, node->as.call.args[i], indent + 2);
        }
        print_indent(out, indent);
        fputs(")\n", out);
        break;
    case CH_IR_SET:
        fprintf(out, " %s\n", node->as.set_expr.name ? node->as.set_expr.name->name : "?");
        ch_ir_print(out, node->as.set_expr.value, indent + 2);
        print_indent(out, indent);
        fputs(")\n", out);
        break;
    case CH_IR_DEFINE:
        fputc(' ', out);
        ch_print_value(out, node->as.define_expr.target, false);
        fputs("\n", out);
        ch_ir_print(out, node->as.define_expr.value, indent + 2);
        print_indent(out, indent);
        fputs(")\n", out);
        break;
    case CH_IR_DEFINE_SYNTAX:
        fputs(" ...)\n", out);
        break;
    case CH_IR_PRIM_CALL:
        fprintf(out, " %s\n",
                node->as.prim_call.symbol ? node->as.prim_call.symbol->name : "?");
        for (size_t i = 0; i < node->as.prim_call.arg_count; i++) {
            ch_ir_print(out, node->as.prim_call.args[i], indent + 2);
        }
        print_indent(out, indent);
        fputs(")\n", out);
        break;
    case CH_IR_RAW:
        fputc(' ', out);
        ch_print_value(out, node->as.raw.expr, false);
        fputs(")\n", out);
        break;
    }
}

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

static bool all_emittable(ChIrNode **items, size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (!ch_ir_llvm_emittable(items[i])) {
            return false;
        }
    }
    return true;
}

bool ch_ir_llvm_emittable(const ChIrNode *node) {
    if (!node) {
        return true;
    }
    switch (node->kind) {
    case CH_IR_VOID:
        return true;
    case CH_IR_LITERAL:
        return ch_is_fixnum(node->as.literal) || node->as.literal == CH_TRUE ||
               node->as.literal == CH_FALSE || ch_is_nil(node->as.literal) ||
               node->as.literal == CH_VOID;
    case CH_IR_VAR:
        return true;
    case CH_IR_IF:
        return ch_ir_llvm_emittable(node->as.if_expr.test) &&
               ch_ir_llvm_emittable(node->as.if_expr.consequent) &&
               (!node->as.if_expr.has_alternate ||
                ch_ir_llvm_emittable(node->as.if_expr.alternate));
    case CH_IR_SEQ:
        return all_emittable(node->as.seq.items, node->as.seq.count);
    case CH_IR_AND:
        return all_emittable(node->as.and_expr.items, node->as.and_expr.count);
    case CH_IR_OR:
        return all_emittable(node->as.or_expr.items, node->as.or_expr.count);
    case CH_IR_PRIM_CALL: {
        ChIrPrim p = node->as.prim_call.prim;
        if (p != CH_IR_PRIM_ADD && p != CH_IR_PRIM_SUB && p != CH_IR_PRIM_MUL &&
            p != CH_IR_PRIM_LT && p != CH_IR_PRIM_NUM_EQ && p != CH_IR_PRIM_NOT) {
            return false;
        }
        return all_emittable(node->as.prim_call.args, node->as.prim_call.arg_count);
    }
    case CH_IR_DEFINE:
        /* MVP: define of an immediately emittable non-lambda value. */
        return ch_is_symbol(node->as.define_expr.target) && node->as.define_expr.value &&
               node->as.define_expr.value->kind != CH_IR_LAMBDA &&
               ch_ir_llvm_emittable(node->as.define_expr.value);
    case CH_IR_CALL:
    case CH_IR_LAMBDA:
    case CH_IR_QUOTE:
    case CH_IR_SET:
    case CH_IR_DEFINE_SYNTAX:
    case CH_IR_RAW:
        /* Closures/calls/raw → eval-fallback (Kaappi whole-scope rule). */
        return false;
    }
    return false;
}

bool ch_ir_llvm_needs_fallback(const ChIrNode *node) {
    return !ch_ir_llvm_emittable(node);
}
