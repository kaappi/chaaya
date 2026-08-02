#include "chaaya/ir.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

typedef struct ChIrOptCtx {
    bool prim_disabled[CH_IR_PRIM_COUNT];
    int lambda_depth;
} ChIrOptCtx;

static bool is_truthy(ChValue v) {
    return v != CH_FALSE;
}

static bool is_fixnum_const(ChIrNode *node, int64_t value) {
    return node && node->is_constant && ch_is_fixnum(node->constant_value) &&
           ch_to_fixnum(node->constant_value) == value;
}

static bool is_const_exact_integer(ChIrNode *node) {
    return node && node->is_constant && ch_is_exact_integer(node->constant_value);
}

static bool fits_fixnum(int64_t v) {
    return v >= CH_FIXNUM_MIN && v <= CH_FIXNUM_MAX;
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

static const char *prim_builtin_name(ChIrPrim prim) {
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

static bool vm_binding_is_builtin_primitive(ChVM *vm, ChIrPrim prim) {
    const char *name = prim_builtin_name(prim);
    if (!name) {
        return false;
    }
    for (size_t i = 0; i < vm->global_count; i++) {
        ChGlobal *global = &vm->globals[i];
        if (!global->name || !global->defined) {
            continue;
        }
        if (strcmp(ch_symbol_basename(global->name), name) != 0) {
            continue;
        }
        if (!ch_is_native(global->value)) {
            return false;
        }
        ChNative *native = ch_as_native(global->value);
        return native->name && strcmp(native->name, name) == 0;
    }
    return false;
}

static void disable_primitive_symbol(ChSymbol *sym, ChIrOptCtx *ctx) {
    ChIrPrim prim = CH_IR_PRIM_ADD;
    if (prim_from_symbol(sym, &prim)) {
        ctx->prim_disabled[prim] = true;
    }
}

static void maybe_collect_redef_from_raw(ChValue expr, int depth, ChIrOptCtx *ctx) {
    if (depth != 0 || !ch_is_pair(expr) || !ch_is_symbol(ch_car(expr))) {
        return;
    }
    ChValue head = ch_car(expr);
    ChValue args = ch_cdr(expr);
    const char *base = ch_symbol_basename(ch_as_symbol(head));
    if (strcmp(base, "define") == 0) {
        if (!ch_is_pair(args)) {
            return;
        }
        ChValue target = ch_car(args);
        if (ch_is_symbol(target)) {
            disable_primitive_symbol(ch_as_symbol(target), ctx);
            return;
        }
        if (ch_is_pair(target) && ch_is_symbol(ch_car(target))) {
            disable_primitive_symbol(ch_as_symbol(ch_car(target)), ctx);
        }
        return;
    }
    if (strcmp(base, "set!") == 0) {
        if (!ch_is_pair(args) || !ch_is_symbol(ch_car(args))) {
            return;
        }
        disable_primitive_symbol(ch_as_symbol(ch_car(args)), ctx);
    }
}

static void collect_redefinitions(ChIrNode *node, ChIrOptCtx *ctx, int depth) {
    if (!node) {
        return;
    }
    switch (node->kind) {
    case CH_IR_IF:
        collect_redefinitions(node->as.if_expr.test, ctx, depth);
        collect_redefinitions(node->as.if_expr.consequent, ctx, depth);
        collect_redefinitions(node->as.if_expr.alternate, ctx, depth);
        return;
    case CH_IR_LAMBDA:
        for (size_t i = 0; i < node->as.lambda.body_count; i++) {
            collect_redefinitions(node->as.lambda.body[i], ctx, depth + 1);
        }
        return;
    case CH_IR_SEQ:
        for (size_t i = 0; i < node->as.seq.count; i++) {
            collect_redefinitions(node->as.seq.items[i], ctx, depth);
        }
        return;
    case CH_IR_CALL:
        collect_redefinitions(node->as.call.callee, ctx, depth);
        for (size_t i = 0; i < node->as.call.arg_count; i++) {
            collect_redefinitions(node->as.call.args[i], ctx, depth);
        }
        return;
    case CH_IR_SET:
        if (depth == 0) {
            disable_primitive_symbol(node->as.set_expr.name, ctx);
        }
        collect_redefinitions(node->as.set_expr.value, ctx, depth);
        return;
    case CH_IR_DEFINE:
        if (depth == 0 && ch_is_symbol(node->as.define_expr.target)) {
            disable_primitive_symbol(ch_as_symbol(node->as.define_expr.target), ctx);
        }
        collect_redefinitions(node->as.define_expr.value, ctx, depth);
        return;
    case CH_IR_AND:
        for (size_t i = 0; i < node->as.and_expr.count; i++) {
            collect_redefinitions(node->as.and_expr.items[i], ctx, depth);
        }
        return;
    case CH_IR_OR:
        for (size_t i = 0; i < node->as.or_expr.count; i++) {
            collect_redefinitions(node->as.or_expr.items[i], ctx, depth);
        }
        return;
    case CH_IR_PRIM_CALL:
        for (size_t i = 0; i < node->as.prim_call.arg_count; i++) {
            collect_redefinitions(node->as.prim_call.args[i], ctx, depth);
        }
        return;
    case CH_IR_RAW:
        maybe_collect_redef_from_raw(node->as.raw.expr, depth, ctx);
        return;
    case CH_IR_VOID:
    case CH_IR_LITERAL:
    case CH_IR_QUOTE:
    case CH_IR_VAR:
    case CH_IR_DEFINE_SYNTAX:
        return;
    }
}

static ChIrNode *make_literal_node(ChValue value) {
    ChIrNode *node = NULL;
    if (value == CH_VOID) {
        node = ch_ir_new_node(CH_IR_VOID);
    } else {
        node = ch_ir_new_node(CH_IR_LITERAL);
        node->as.literal = value;
    }
    node->is_constant = true;
    node->constant_value = value;
    return node;
}

static bool safe_add_i64(int64_t a, int64_t b, int64_t *out) {
    if ((b > 0 && a > INT64_MAX - b) || (b < 0 && a < INT64_MIN - b)) {
        return false;
    }
    *out = a + b;
    return true;
}

static bool safe_mul_i64(int64_t a, int64_t b, int64_t *out) {
    if (a == 0 || b == 0) {
        *out = 0;
        return true;
    }
    if (a > 0) {
        if (b > 0) {
            if (a > INT64_MAX / b) {
                return false;
            }
        } else {
            if (b < INT64_MIN / a) {
                return false;
            }
        }
    } else {
        if (b > 0) {
            if (a < INT64_MIN / b) {
                return false;
            }
        } else {
            if (a != 0 && b < INT64_MAX / a) {
                return false;
            }
        }
    }
    if ((a == -1 && b == INT64_MIN) || (b == -1 && a == INT64_MIN)) {
        return false;
    }
    *out = a * b;
    return true;
}

static bool as_number(ChValue v, double *out_double, int64_t *out_int, bool *is_fixnum) {
    if (ch_is_fixnum(v)) {
        int64_t i = ch_to_fixnum(v);
        *out_double = (double)i;
        *out_int = i;
        *is_fixnum = true;
        return true;
    }
    if (ch_is_flonum(v)) {
        *out_double = ch_to_flonum(v);
        *out_int = 0;
        *is_fixnum = false;
        return true;
    }
    return false;
}

static bool fold_prim_call(const ChIrPrimCallExpr *prim_call, ChValue *out_value) {
    size_t nargs = prim_call->arg_count;
    ChValue values[16];
    bool all_fixnum = true;
    if (nargs > 16) {
        return false;
    }
    for (size_t i = 0; i < nargs; i++) {
        ChIrNode *arg = prim_call->args[i];
        if (!arg || !arg->is_constant) {
            return false;
        }
        values[i] = arg->constant_value;
        if (!ch_is_fixnum(values[i])) {
            all_fixnum = false;
        }
    }

    switch (prim_call->prim) {
    case CH_IR_PRIM_ADD: {
        if (all_fixnum) {
            int64_t sum = 0;
            for (size_t i = 0; i < nargs; i++) {
                if (!safe_add_i64(sum, ch_to_fixnum(values[i]), &sum)) {
                    return false;
                }
            }
            if (!fits_fixnum(sum)) {
                return false;
            }
            *out_value = ch_make_fixnum(sum);
            return true;
        }
        double sum = 0.0;
        for (size_t i = 0; i < nargs; i++) {
            double d = 0.0;
            int64_t ignored = 0;
            bool is_fix = false;
            if (!as_number(values[i], &d, &ignored, &is_fix)) {
                return false;
            }
            sum += d;
        }
        *out_value = ch_make_flonum(sum);
        return true;
    }
    case CH_IR_PRIM_SUB: {
        if (nargs == 0) {
            return false;
        }
        if (all_fixnum) {
            int64_t result = ch_to_fixnum(values[0]);
            if (nargs == 1) {
                result = -result;
            } else {
                for (size_t i = 1; i < nargs; i++) {
                    if (!safe_add_i64(result, -ch_to_fixnum(values[i]), &result)) {
                        return false;
                    }
                }
            }
            if (!fits_fixnum(result)) {
                return false;
            }
            *out_value = ch_make_fixnum(result);
            return true;
        }
        double result = 0.0;
        int64_t ignored = 0;
        bool is_fix = false;
        if (!as_number(values[0], &result, &ignored, &is_fix)) {
            return false;
        }
        if (nargs == 1) {
            result = -result;
        } else {
            for (size_t i = 1; i < nargs; i++) {
                double d = 0.0;
                if (!as_number(values[i], &d, &ignored, &is_fix)) {
                    return false;
                }
                result -= d;
            }
        }
        *out_value = ch_make_flonum(result);
        return true;
    }
    case CH_IR_PRIM_MUL: {
        if (all_fixnum) {
            int64_t result = 1;
            for (size_t i = 0; i < nargs; i++) {
                if (!safe_mul_i64(result, ch_to_fixnum(values[i]), &result)) {
                    return false;
                }
            }
            if (!fits_fixnum(result)) {
                return false;
            }
            *out_value = ch_make_fixnum(result);
            return true;
        }
        double result = 1.0;
        for (size_t i = 0; i < nargs; i++) {
            double d = 0.0;
            int64_t ignored = 0;
            bool is_fix = false;
            if (!as_number(values[i], &d, &ignored, &is_fix)) {
                return false;
            }
            result *= d;
        }
        *out_value = ch_make_flonum(result);
        return true;
    }
    case CH_IR_PRIM_LT:
    case CH_IR_PRIM_GT:
    case CH_IR_PRIM_NUM_EQ:
    case CH_IR_PRIM_LE:
    case CH_IR_PRIM_GE: {
        bool ok = true;
        if (nargs <= 1) {
            *out_value = CH_TRUE;
            return true;
        }
        double prev = 0.0;
        int64_t ignored = 0;
        bool is_fix = false;
        if (!as_number(values[0], &prev, &ignored, &is_fix)) {
            return false;
        }
        for (size_t i = 1; i < nargs; i++) {
            double cur = 0.0;
            if (!as_number(values[i], &cur, &ignored, &is_fix)) {
                return false;
            }
            switch (prim_call->prim) {
            case CH_IR_PRIM_LT:
                ok = prev < cur;
                break;
            case CH_IR_PRIM_GT:
                ok = prev > cur;
                break;
            case CH_IR_PRIM_NUM_EQ:
                ok = prev == cur;
                break;
            case CH_IR_PRIM_LE:
                ok = prev <= cur;
                break;
            case CH_IR_PRIM_GE:
                ok = prev >= cur;
                break;
            default:
                ok = false;
                break;
            }
            if (!ok) {
                *out_value = CH_FALSE;
                return true;
            }
            prev = cur;
        }
        *out_value = CH_TRUE;
        return true;
    }
    case CH_IR_PRIM_NOT:
        if (nargs != 1) {
            return false;
        }
        *out_value = (values[0] == CH_FALSE) ? CH_TRUE : CH_FALSE;
        return true;
    case CH_IR_PRIM_COUNT:
        return false;
    }
    return false;
}

static ChIrNode *detach_and_keep_prim_arg(ChIrNode *node, size_t keep_index) {
    ChIrNode *kept = node->as.prim_call.args[keep_index];
    node->as.prim_call.args[keep_index] = NULL;
    for (size_t i = 0; i < node->as.prim_call.arg_count; i++) {
        if (i == keep_index) {
            continue;
        }
        ch_ir_free(node->as.prim_call.args[i]);
    }
    free(node->as.prim_call.args);
    free(node);
    return kept;
}

static ChIrNode *simplify_prim_identity(ChIrNode *node) {
    ChIrPrimCallExpr *pc = &node->as.prim_call;
    if (pc->arg_count != 2) {
        return node;
    }
    switch (pc->prim) {
    case CH_IR_PRIM_ADD:
        if (is_fixnum_const(pc->args[0], 0) && is_const_exact_integer(pc->args[1])) {
            return detach_and_keep_prim_arg(node, 1);
        }
        if (is_fixnum_const(pc->args[1], 0) && is_const_exact_integer(pc->args[0])) {
            return detach_and_keep_prim_arg(node, 0);
        }
        return node;
    case CH_IR_PRIM_SUB:
        if (is_fixnum_const(pc->args[1], 0) && is_const_exact_integer(pc->args[0])) {
            return detach_and_keep_prim_arg(node, 0);
        }
        return node;
    case CH_IR_PRIM_MUL:
        if (is_fixnum_const(pc->args[0], 1) && is_const_exact_integer(pc->args[1])) {
            return detach_and_keep_prim_arg(node, 1);
        }
        if (is_fixnum_const(pc->args[1], 1) && is_const_exact_integer(pc->args[0])) {
            return detach_and_keep_prim_arg(node, 0);
        }
        return node;
    case CH_IR_PRIM_LT:
    case CH_IR_PRIM_GT:
    case CH_IR_PRIM_NUM_EQ:
    case CH_IR_PRIM_LE:
    case CH_IR_PRIM_GE:
    case CH_IR_PRIM_NOT:
    case CH_IR_PRIM_COUNT:
        return node;
    }
    return node;
}

static ChIrNode *optimize_node(ChCompiler *c, ChIrNode *node, ChIrOptCtx *ctx);

static ChIrNode *optimize_seq_node(ChCompiler *c, ChIrNode *node, ChIrOptCtx *ctx) {
    for (size_t i = 0; i < node->as.seq.count; i++) {
        node->as.seq.items[i] = optimize_node(c, node->as.seq.items[i], ctx);
    }

    size_t flat_count = 0;
    bool needs_flatten = false;
    for (size_t i = 0; i < node->as.seq.count; i++) {
        ChIrNode *child = node->as.seq.items[i];
        if (child && child->kind == CH_IR_SEQ) {
            needs_flatten = true;
            flat_count += child->as.seq.count;
        } else {
            flat_count++;
        }
    }

    if (needs_flatten) {
        ChIrNode **flat = NULL;
        if (flat_count > 0) {
            flat = (ChIrNode **)calloc(flat_count, sizeof(*flat));
            if (!flat) {
                abort();
            }
        }
        size_t at = 0;
        for (size_t i = 0; i < node->as.seq.count; i++) {
            ChIrNode *child = node->as.seq.items[i];
            if (child && child->kind == CH_IR_SEQ) {
                for (size_t j = 0; j < child->as.seq.count; j++) {
                    flat[at++] = child->as.seq.items[j];
                    child->as.seq.items[j] = NULL;
                }
                free(child->as.seq.items);
                free(child);
            } else {
                flat[at++] = child;
            }
        }
        free(node->as.seq.items);
        node->as.seq.items = flat;
        node->as.seq.count = flat_count;
    }

    if (node->as.seq.count == 0) {
        free(node->as.seq.items);
        free(node);
        return make_literal_node(CH_VOID);
    }
    if (node->as.seq.count == 1) {
        ChIrNode *single = node->as.seq.items[0];
        node->as.seq.items[0] = NULL;
        free(node->as.seq.items);
        free(node);
        return single;
    }
    return node;
}

static ChIrNode *optimize_if_node(ChCompiler *c, ChIrNode *node, ChIrOptCtx *ctx) {
    node->as.if_expr.test = optimize_node(c, node->as.if_expr.test, ctx);
    node->as.if_expr.consequent = optimize_node(c, node->as.if_expr.consequent, ctx);
    if (node->as.if_expr.has_alternate) {
        node->as.if_expr.alternate = optimize_node(c, node->as.if_expr.alternate, ctx);
    }

    if (ctx->lambda_depth == 0 && node->as.if_expr.test &&
        node->as.if_expr.test->kind == CH_IR_PRIM_CALL &&
        node->as.if_expr.test->as.prim_call.prim == CH_IR_PRIM_NOT &&
        !ctx->prim_disabled[CH_IR_PRIM_NOT] &&
        node->as.if_expr.test->as.prim_call.arg_count == 1) {
        ChIrNode *not_call = node->as.if_expr.test;
        ChIrNode *arg = not_call->as.prim_call.args[0];
        not_call->as.prim_call.args[0] = NULL;
        ch_ir_free(not_call);
        node->as.if_expr.test = arg;
        if (node->as.if_expr.has_alternate) {
            ChIrNode *tmp = node->as.if_expr.consequent;
            node->as.if_expr.consequent = node->as.if_expr.alternate;
            node->as.if_expr.alternate = tmp;
        } else {
            ChIrNode *orig = node->as.if_expr.consequent;
            node->as.if_expr.consequent = make_literal_node(CH_FALSE);
            node->as.if_expr.alternate = orig;
            node->as.if_expr.has_alternate = true;
        }
    }

    if (node->as.if_expr.test && node->as.if_expr.test->is_constant) {
        bool cond = is_truthy(node->as.if_expr.test->constant_value);
        ChIrNode *chosen = NULL;
        ch_ir_free(node->as.if_expr.test);
        node->as.if_expr.test = NULL;
        if (cond) {
            chosen = node->as.if_expr.consequent;
            node->as.if_expr.consequent = NULL;
            ch_ir_free(node->as.if_expr.alternate);
            node->as.if_expr.alternate = NULL;
        } else if (node->as.if_expr.has_alternate) {
            chosen = node->as.if_expr.alternate;
            node->as.if_expr.alternate = NULL;
            ch_ir_free(node->as.if_expr.consequent);
            node->as.if_expr.consequent = NULL;
        } else {
            ch_ir_free(node->as.if_expr.consequent);
            node->as.if_expr.consequent = NULL;
            chosen = make_literal_node(CH_FALSE);
        }
        free(node);
        return chosen;
    }
    return node;
}

static ChIrNode *optimize_prim_call_node(ChIrNode *node, ChIrOptCtx *ctx) {
    ChIrPrimCallExpr *pc = &node->as.prim_call;
    for (size_t i = 0; i < pc->arg_count; i++) {
        pc->args[i] = optimize_node(NULL, pc->args[i], ctx);
    }

    if (ctx->lambda_depth > 0 || ctx->prim_disabled[pc->prim]) {
        return node;
    }

    ChValue folded = CH_UNDEFINED;
    if (fold_prim_call(pc, &folded)) {
        ch_ir_free(node);
        return make_literal_node(folded);
    }

    return simplify_prim_identity(node);
}

static ChIrNode *optimize_node(ChCompiler *c, ChIrNode *node, ChIrOptCtx *ctx) {
    (void)c;
    if (!node) {
        return NULL;
    }
    switch (node->kind) {
    case CH_IR_IF:
        return optimize_if_node(c, node, ctx);
    case CH_IR_LAMBDA:
        ctx->lambda_depth++;
        for (size_t i = 0; i < node->as.lambda.body_count; i++) {
            node->as.lambda.body[i] = optimize_node(c, node->as.lambda.body[i], ctx);
        }
        ctx->lambda_depth--;
        return node;
    case CH_IR_SEQ:
        return optimize_seq_node(c, node, ctx);
    case CH_IR_CALL:
        node->as.call.callee = optimize_node(c, node->as.call.callee, ctx);
        for (size_t i = 0; i < node->as.call.arg_count; i++) {
            node->as.call.args[i] = optimize_node(c, node->as.call.args[i], ctx);
        }
        return node;
    case CH_IR_SET:
        node->as.set_expr.value = optimize_node(c, node->as.set_expr.value, ctx);
        return node;
    case CH_IR_DEFINE:
        node->as.define_expr.value = optimize_node(c, node->as.define_expr.value, ctx);
        return node;
    case CH_IR_AND:
        for (size_t i = 0; i < node->as.and_expr.count; i++) {
            node->as.and_expr.items[i] = optimize_node(c, node->as.and_expr.items[i], ctx);
        }
        return node;
    case CH_IR_OR:
        for (size_t i = 0; i < node->as.or_expr.count; i++) {
            node->as.or_expr.items[i] = optimize_node(c, node->as.or_expr.items[i], ctx);
        }
        return node;
    case CH_IR_PRIM_CALL:
        return optimize_prim_call_node(node, ctx);
    case CH_IR_VOID:
    case CH_IR_LITERAL:
    case CH_IR_QUOTE:
    case CH_IR_VAR:
    case CH_IR_DEFINE_SYNTAX:
    case CH_IR_RAW:
        return node;
    }
    return node;
}

ChCompileStatus ch_ir_optimize(ChCompiler *c, ChIrNode **root_slot) {
    if (!root_slot || !*root_slot) {
        return CH_COMPILE_OK;
    }

    ChIrOptCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    /* Inlining assumes the arithmetic names resolve to the ambient VM globals;
     * that assumption doesn't hold when compiling for a specific eval
     * environment (e.g. (null-environment 5)), which may deliberately hide or
     * shadow them. Fall back to normal variable lookup in that case so the
     * environment's own visibility rules (compile_variable) are honored. */
    if (!c || !c->vm || c->vm->active_lib_env) {
        for (size_t i = 0; i < CH_IR_PRIM_COUNT; i++) {
            ctx.prim_disabled[i] = true;
        }
    } else {
        for (size_t i = 0; i < CH_IR_PRIM_COUNT; i++) {
            if (!vm_binding_is_builtin_primitive(c->vm, (ChIrPrim)i)) {
                ctx.prim_disabled[i] = true;
            }
        }
    }
    collect_redefinitions(*root_slot, &ctx, 0);
    *root_slot = optimize_node(c, *root_slot, &ctx);
    ch_ir_analyze(*root_slot);
    return CH_COMPILE_OK;
}
