#include "chaaya/ir.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool is_symbol_named(ChValue v, const char *name) {
    return ch_is_symbol(v) && strcmp(ch_symbol_basename(ch_as_symbol(v)), name) == 0;
}

static bool is_derived_special_form(ChValue head) {
    if (!ch_is_symbol(head)) {
        return false;
    }
    const char *name = ch_symbol_basename(ch_as_symbol(head));
    static const char *derived[] = {
        "quasiquote",   "let",          "let*",         "letrec",      "letrec*",
        "cond",         "case",         "when",         "unless",      "do",
        "guard",        "parameterize", "case-lambda",  "delay",       "delay-force",
        "let-values",   "let*-values",  "define-values", "define-library", "include",
        "include-ci",   "cond-expand",  "let-syntax",   "letrec-syntax",  "define-property",
    };
    for (size_t i = 0; i < sizeof(derived) / sizeof(derived[0]); i++) {
        if (strcmp(name, derived[i]) == 0) {
            return true;
        }
    }
    return false;
}

static bool prim_from_symbol(ChSymbol *sym, ChIrPrim *out) {
    const char *name = ch_symbol_basename(sym);
    if (strcmp(name, "+") == 0) {
        *out = CH_IR_PRIM_ADD;
        return true;
    }
    if (strcmp(name, "-") == 0) {
        *out = CH_IR_PRIM_SUB;
        return true;
    }
    if (strcmp(name, "*") == 0) {
        *out = CH_IR_PRIM_MUL;
        return true;
    }
    if (strcmp(name, "<") == 0) {
        *out = CH_IR_PRIM_LT;
        return true;
    }
    if (strcmp(name, ">") == 0) {
        *out = CH_IR_PRIM_GT;
        return true;
    }
    if (strcmp(name, "=") == 0) {
        *out = CH_IR_PRIM_NUM_EQ;
        return true;
    }
    if (strcmp(name, "<=") == 0) {
        *out = CH_IR_PRIM_LE;
        return true;
    }
    if (strcmp(name, ">=") == 0) {
        *out = CH_IR_PRIM_GE;
        return true;
    }
    if (strcmp(name, "not") == 0) {
        *out = CH_IR_PRIM_NOT;
        return true;
    }
    return false;
}

static ChCompileStatus lower_node(ChCompiler *c, ChValue expr, ChIrNode **out);

static ChCompileStatus make_raw(ChValue expr, ChIrNode **out) {
    ChIrNode *node = ch_ir_new_node(CH_IR_RAW);
    node->as.raw.expr = expr;
    *out = node;
    return CH_COMPILE_OK;
}

static ChCompileStatus lower_list_nodes(ChCompiler *c, ChValue list, ChIrNode ***out_items,
                                        size_t *out_count) {
    size_t count = 0;
    for (ChValue it = list; ch_is_pair(it); it = ch_cdr(it)) {
        count++;
    }
    {
        ChValue tail = list;
        while (ch_is_pair(tail)) {
            tail = ch_cdr(tail);
        }
        if (!ch_is_nil(tail)) {
            *out_items = NULL;
            *out_count = 0;
            return CH_COMPILE_ERROR;
        }
    }

    ChIrNode **items = NULL;
    if (count > 0) {
        items = (ChIrNode **)calloc(count, sizeof(*items));
        if (!items) {
            abort();
        }
    }

    size_t i = 0;
    for (ChValue it = list; ch_is_pair(it); it = ch_cdr(it), i++) {
        if (lower_node(c, ch_car(it), &items[i]) != CH_COMPILE_OK) {
            for (size_t j = 0; j < i; j++) {
                ch_ir_free(items[j]);
            }
            free(items);
            *out_items = NULL;
            *out_count = 0;
            return CH_COMPILE_ERROR;
        }
    }
    *out_items = items;
    *out_count = count;
    return CH_COMPILE_OK;
}

static ChCompileStatus lower_if(ChCompiler *c, ChValue expr, ChValue args, ChIrNode **out) {
    if (!ch_is_pair(args) || !ch_is_pair(ch_cdr(args))) {
        return make_raw(expr, out);
    }

    ChValue test = ch_car(args);
    ChValue consequent = ch_car(ch_cdr(args));
    ChValue rest = ch_cdr(ch_cdr(args));
    ChValue alternate = CH_FALSE;
    bool has_alt = false;
    if (ch_is_pair(rest)) {
        alternate = ch_car(rest);
        has_alt = true;
        rest = ch_cdr(rest);
    }
    if (!ch_is_nil(rest)) {
        return make_raw(expr, out);
    }

    ChIrNode *test_node = NULL;
    ChIrNode *cons_node = NULL;
    ChIrNode *alt_node = NULL;
    if (lower_node(c, test, &test_node) != CH_COMPILE_OK ||
        lower_node(c, consequent, &cons_node) != CH_COMPILE_OK) {
        ch_ir_free(test_node);
        ch_ir_free(cons_node);
        return CH_COMPILE_ERROR;
    }
    if (has_alt && lower_node(c, alternate, &alt_node) != CH_COMPILE_OK) {
        ch_ir_free(test_node);
        ch_ir_free(cons_node);
        ch_ir_free(alt_node);
        return CH_COMPILE_ERROR;
    }

    ChIrNode *node = ch_ir_new_node(CH_IR_IF);
    node->as.if_expr.test = test_node;
    node->as.if_expr.consequent = cons_node;
    node->as.if_expr.alternate = alt_node;
    node->as.if_expr.has_alternate = has_alt;
    *out = node;
    return CH_COMPILE_OK;
}

static ChCompileStatus lower_lambda(ChCompiler *c, ChValue expr, ChValue args, ChIrNode **out) {
    if (!ch_is_pair(args) || !ch_is_pair(ch_cdr(args))) {
        return make_raw(expr, out);
    }

    ChValue params = ch_car(args);
    ChValue body = ch_cdr(args);
    ChIrNode **body_nodes = NULL;
    size_t body_count = 0;
    if (lower_list_nodes(c, body, &body_nodes, &body_count) != CH_COMPILE_OK) {
        return make_raw(expr, out);
    }

    ChIrNode *node = ch_ir_new_node(CH_IR_LAMBDA);
    node->as.lambda.params = params;
    node->as.lambda.body = body_nodes;
    node->as.lambda.body_count = body_count;
    *out = node;
    return CH_COMPILE_OK;
}

static ChCompileStatus lower_quote(ChValue expr, ChValue args, ChIrNode **out) {
    if (!ch_is_pair(args) || !ch_is_nil(ch_cdr(args))) {
        return make_raw(expr, out);
    }
    ChIrNode *node = ch_ir_new_node(CH_IR_QUOTE);
    node->as.quoted = ch_car(args);
    *out = node;
    return CH_COMPILE_OK;
}

static ChCompileStatus lower_set(ChCompiler *c, ChValue expr, ChValue args, ChIrNode **out) {
    if (!ch_is_pair(args) || !ch_is_pair(ch_cdr(args)) || !ch_is_nil(ch_cdr(ch_cdr(args))) ||
        !ch_is_symbol(ch_car(args))) {
        return make_raw(expr, out);
    }

    ChIrNode *value_node = NULL;
    if (lower_node(c, ch_car(ch_cdr(args)), &value_node) != CH_COMPILE_OK) {
        return CH_COMPILE_ERROR;
    }

    ChIrNode *node = ch_ir_new_node(CH_IR_SET);
    node->as.set_expr.name = ch_as_symbol(ch_car(args));
    node->as.set_expr.value = value_node;
    *out = node;
    return CH_COMPILE_OK;
}

static ChCompileStatus lower_define(ChCompiler *c, ChValue expr, ChValue args, ChIrNode **out) {
    if (!ch_is_pair(args)) {
        return make_raw(expr, out);
    }
    ChValue target = ch_car(args);
    ChValue rest = ch_cdr(args);
    if (!ch_is_symbol(target) || !ch_is_pair(rest) || !ch_is_nil(ch_cdr(rest))) {
        return make_raw(expr, out);
    }

    ChIrNode *value_node = NULL;
    if (lower_node(c, ch_car(rest), &value_node) != CH_COMPILE_OK) {
        return CH_COMPILE_ERROR;
    }

    ChIrNode *node = ch_ir_new_node(CH_IR_DEFINE);
    node->as.define_expr.target = target;
    node->as.define_expr.value = value_node;
    *out = node;
    return CH_COMPILE_OK;
}

static ChCompileStatus lower_define_syntax(ChValue expr, ChIrNode **out) {
    (void)expr;
    ChIrNode *node = ch_ir_new_node(CH_IR_DEFINE_SYNTAX);
    *out = node;
    return CH_COMPILE_OK;
}

static ChCompileStatus lower_variadic_seq(ChCompiler *c, ChValue expr, ChValue args, ChIrKind kind,
                                          ChIrNode **out) {
    ChIrNode **items = NULL;
    size_t count = 0;
    if (lower_list_nodes(c, args, &items, &count) != CH_COMPILE_OK) {
        return make_raw(expr, out);
    }

    ChIrNode *node = ch_ir_new_node(kind);
    if (kind == CH_IR_SEQ) {
        node->as.seq.items = items;
        node->as.seq.count = count;
    } else if (kind == CH_IR_AND) {
        node->as.and_expr.items = items;
        node->as.and_expr.count = count;
    } else {
        node->as.or_expr.items = items;
        node->as.or_expr.count = count;
    }
    *out = node;
    return CH_COMPILE_OK;
}

static ChCompileStatus lower_call_like(ChCompiler *c, ChValue expr, ChValue callee_value, ChValue args,
                                       ChIrNode **out) {
    ChIrNode **arg_nodes = NULL;
    size_t arg_count = 0;
    if (lower_list_nodes(c, args, &arg_nodes, &arg_count) != CH_COMPILE_OK) {
        return make_raw(expr, out);
    }

    if (ch_is_symbol(callee_value)) {
        ChIrPrim prim = CH_IR_PRIM_ADD;
        if (prim_from_symbol(ch_as_symbol(callee_value), &prim)) {
            ChIrNode *node = ch_ir_new_node(CH_IR_PRIM_CALL);
            node->as.prim_call.prim = prim;
            node->as.prim_call.symbol = ch_as_symbol(callee_value);
            node->as.prim_call.args = arg_nodes;
            node->as.prim_call.arg_count = arg_count;
            *out = node;
            return CH_COMPILE_OK;
        }
    }

    ChIrNode *callee = NULL;
    if (lower_node(c, callee_value, &callee) != CH_COMPILE_OK) {
        for (size_t i = 0; i < arg_count; i++) {
            ch_ir_free(arg_nodes[i]);
        }
        free(arg_nodes);
        return CH_COMPILE_ERROR;
    }

    ChIrNode *node = ch_ir_new_node(CH_IR_CALL);
    node->as.call.callee = callee;
    node->as.call.args = arg_nodes;
    node->as.call.arg_count = arg_count;
    *out = node;
    return CH_COMPILE_OK;
}

static ChCompileStatus lower_node(ChCompiler *c, ChValue expr, ChIrNode **out) {
    if (expr == CH_VOID) {
        *out = ch_ir_new_node(CH_IR_VOID);
        return CH_COMPILE_OK;
    }
    if (ch_is_symbol(expr)) {
        ChIrNode *node = ch_ir_new_node(CH_IR_VAR);
        node->as.var = ch_as_symbol(expr);
        *out = node;
        return CH_COMPILE_OK;
    }
    if (!ch_is_pair(expr)) {
        ChIrNode *node = ch_ir_new_node(CH_IR_LITERAL);
        node->as.literal = expr;
        *out = node;
        return CH_COMPILE_OK;
    }

    ChValue head = ch_car(expr);
    ChValue args = ch_cdr(expr);
    if (is_symbol_named(head, "quote")) {
        return lower_quote(expr, args, out);
    }
    if (is_symbol_named(head, "if")) {
        return lower_if(c, expr, args, out);
    }
    if (is_symbol_named(head, "begin")) {
        return lower_variadic_seq(c, expr, args, CH_IR_SEQ, out);
    }
    if (is_symbol_named(head, "lambda")) {
        return lower_lambda(c, expr, args, out);
    }
    if (is_symbol_named(head, "set!")) {
        return lower_set(c, expr, args, out);
    }
    if (is_symbol_named(head, "define")) {
        return lower_define(c, expr, args, out);
    }
    if (is_symbol_named(head, "define-syntax")) {
        return lower_define_syntax(expr, out);
    }
    if (is_symbol_named(head, "and")) {
        return lower_variadic_seq(c, expr, args, CH_IR_AND, out);
    }
    if (is_symbol_named(head, "or")) {
        return lower_variadic_seq(c, expr, args, CH_IR_OR, out);
    }
    if (is_derived_special_form(head)) {
        return make_raw(expr, out);
    }
    return lower_call_like(c, expr, head, args, out);
}

ChCompileStatus ch_ir_lower(ChCompiler *c, ChValue expr, ChIrNode **out_root) {
    if (!out_root) {
        snprintf(c->error, sizeof(c->error), "ir lower: missing output");
        return CH_COMPILE_ERROR;
    }
    *out_root = NULL;
    return lower_node(c, expr, out_root);
}
