#include "chaaya/llvm_backend.h"

#include "chaaya/cli.h"
#include "chaaya/compiler.h"
#include "chaaya/eval.h"
#include "chaaya/expander.h"
#include "chaaya/ir.h"
#include "chaaya/reader.h"
#include "chaaya/runtime_exports.h"
#include "chaaya/version.h"
#include "chaaya/vm.h"

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#ifndef CHAAYA_SOURCE_DIR
#define CHAAYA_SOURCE_DIR "."
#endif
#ifndef CHAAYA_BUILD_DIR
#define CHAAYA_BUILD_DIR "."
#endif

typedef struct {
    FILE *out;
    int next_id;
    bool ok;
} EmitCtx;

static void sanitize_sym(const char *name, char *out, size_t out_sz);

static bool checked_add_i64(int64_t a, int64_t b, int64_t *out) {
    if ((b > 0 && a > INT64_MAX - b) || (b < 0 && a < INT64_MIN - b)) {
        return false;
    }
    *out = a + b;
    return true;
}

static bool checked_sub_i64(int64_t a, int64_t b, int64_t *out) {
    if ((b > 0 && a < INT64_MIN + b) || (b < 0 && a > INT64_MAX + b)) {
        return false;
    }
    *out = a - b;
    return true;
}

static bool checked_mul_i64(int64_t a, int64_t b, int64_t *out) {
    if (a == 0 || b == 0) {
        *out = 0;
        return true;
    }
    if ((a == INT64_MIN && b == -1) || (a == -1 && b == INT64_MIN)) {
        return false;
    }
    if (a > 0) {
        if ((b > 0 && a > INT64_MAX / b) || (b < 0 && b < INT64_MIN / a)) {
            return false;
        }
    } else {
        if ((b > 0 && a < INT64_MIN / b) || (b < 0 && a < INT64_MAX / b)) {
            return false;
        }
    }
    *out = a * b;
    return true;
}

static const char *host_triple(void) {
#if defined(__APPLE__) && defined(__aarch64__)
    return "arm64-apple-macosx";
#elif defined(__APPLE__) && (defined(__x86_64__) || defined(__amd64__))
    return "x86_64-apple-macosx";
#elif defined(__linux__) && defined(__aarch64__)
    return "aarch64-unknown-linux-gnu";
#elif defined(__linux__) && (defined(__x86_64__) || defined(__amd64__))
    return "x86_64-unknown-linux-gnu";
#else
    return "unknown-unknown-unknown";
#endif
}

static bool source_has_import(const char *src, size_t len) {
    if (!src) {
        return false;
    }
    for (size_t i = 0; i + 6 < len; i++) {
        if (src[i] == '(' && strncmp(src + i + 1, "import", 6) == 0) {
            char next = src[i + 7];
            if (next == ' ' || next == '\t' || next == '\n' || next == '\r' || next == '(') {
                return true;
            }
        }
    }
    return false;
}

static bool ir_eval_fixnum_expr(const ChIrNode *node, int64_t *out) {
    if (!node || !out) {
        return false;
    }
    if (node->is_constant && ch_is_fixnum(node->constant_value)) {
        *out = ch_to_fixnum(node->constant_value);
        return true;
    }
    switch (node->kind) {
    case CH_IR_LITERAL:
        if (ch_is_fixnum(node->as.literal)) {
            *out = ch_to_fixnum(node->as.literal);
            return true;
        }
        return false;
    case CH_IR_SEQ:
        if (node->as.seq.count == 0) {
            return false;
        }
        return ir_eval_fixnum_expr(node->as.seq.items[node->as.seq.count - 1], out);
    case CH_IR_PRIM_CALL: {
        ChIrPrim prim = node->as.prim_call.prim;
        size_t argc = node->as.prim_call.arg_count;
        if (prim == CH_IR_PRIM_ADD) {
            int64_t acc = 0;
            for (size_t i = 0; i < argc; i++) {
                int64_t part = 0;
                if (!ir_eval_fixnum_expr(node->as.prim_call.args[i], &part) ||
                    !checked_add_i64(acc, part, &acc)) {
                    return false;
                }
            }
            *out = acc;
            return true;
        }
        if (prim == CH_IR_PRIM_SUB) {
            if (argc == 0) {
                return false;
            }
            int64_t acc = 0;
            if (!ir_eval_fixnum_expr(node->as.prim_call.args[0], &acc)) {
                return false;
            }
            if (argc == 1) {
                return checked_sub_i64(0, acc, out);
            }
            for (size_t i = 1; i < argc; i++) {
                int64_t part = 0;
                if (!ir_eval_fixnum_expr(node->as.prim_call.args[i], &part) ||
                    !checked_sub_i64(acc, part, &acc)) {
                    return false;
                }
            }
            *out = acc;
            return true;
        }
        if (prim == CH_IR_PRIM_MUL) {
            int64_t acc = 1;
            for (size_t i = 0; i < argc; i++) {
                int64_t part = 0;
                if (!ir_eval_fixnum_expr(node->as.prim_call.args[i], &part) ||
                    !checked_mul_i64(acc, part, &acc)) {
                    return false;
                }
            }
            *out = acc;
            return true;
        }
        return false;
    }
    default:
        return false;
    }
}

static int fresh(EmitCtx *ctx) {
    return ctx->next_id++;
}

static int emit_value(EmitCtx *ctx, const ChIrNode *node);

static const char *prim_rt_name(ChIrPrim prim) {
    switch (prim) {
    case CH_IR_PRIM_ADD:
        return "ch_rt_fixnum_add";
    case CH_IR_PRIM_SUB:
        return "ch_rt_fixnum_sub";
    case CH_IR_PRIM_MUL:
        return "ch_rt_fixnum_mul";
    case CH_IR_PRIM_LT:
        return "ch_rt_fixnum_lt";
    case CH_IR_PRIM_NUM_EQ:
        return "ch_rt_fixnum_eq";
    default:
        return NULL;
    }
}

static int emit_value(EmitCtx *ctx, const ChIrNode *node) {
    if (!ctx->ok || !node) {
        ctx->ok = false;
        return -1;
    }
    switch (node->kind) {
    case CH_IR_VOID: {
        int id = fresh(ctx);
        fprintf(ctx->out, "  %%v%d = add i64 0, %llu\n", id, (unsigned long long)CH_VOID);
        return id;
    }
    case CH_IR_LITERAL: {
        int id = fresh(ctx);
        fprintf(ctx->out, "  %%v%d = add i64 0, %llu\n", id,
                (unsigned long long)node->as.literal);
        return id;
    }
    case CH_IR_VAR: {
        const char *name = node->as.var ? ch_symbol_basename(node->as.var) : NULL;
        if (!name) {
            ctx->ok = false;
            return -1;
        }
        char san[128];
        sanitize_sym(name, san, sizeof(san));
        int id = fresh(ctx);
        int name_id = fresh(ctx);
        fprintf(ctx->out, "  %%n%d = getelementptr inbounds [%zu x i8], ptr @.name_%s, i64 0, i64 0\n",
                name_id, strlen(name) + 1, san);
        fprintf(ctx->out, "  %%v%d = call i64 @ch_rt_global_lookup(ptr %%vm, ptr %%n%d, i64 %zu)\n",
                id, name_id, strlen(name));
        return id;
    }
    case CH_IR_SEQ: {
        int last = -1;
        for (size_t i = 0; i < node->as.seq.count; i++) {
            last = emit_value(ctx, node->as.seq.items[i]);
            if (last < 0) {
                return -1;
            }
        }
        if (last < 0) {
            int id = fresh(ctx);
            fprintf(ctx->out, "  %%v%d = add i64 0, %llu\n", id, (unsigned long long)CH_VOID);
            return id;
        }
        return last;
    }
    case CH_IR_IF: {
        int test = emit_value(ctx, node->as.if_expr.test);
        if (test < 0) {
            return -1;
        }
        int then_l = fresh(ctx);
        int else_l = fresh(ctx);
        int join_l = fresh(ctx);
        int cmp = fresh(ctx);
        fprintf(ctx->out, "  %%c%d = icmp ne i64 %%v%d, %llu\n", cmp, test,
                (unsigned long long)CH_FALSE);
        fprintf(ctx->out, "  br i1 %%c%d, label %%L%d, label %%L%d\n", cmp, then_l, else_l);
        fprintf(ctx->out, "L%d:\n", then_l);
        int then_v = emit_value(ctx, node->as.if_expr.consequent);
        if (then_v < 0) {
            return -1;
        }
        fprintf(ctx->out, "  br label %%L%d\n", join_l);
        fprintf(ctx->out, "L%d:\n", else_l);
        int else_v;
        if (node->as.if_expr.has_alternate) {
            else_v = emit_value(ctx, node->as.if_expr.alternate);
        } else {
            else_v = fresh(ctx);
            fprintf(ctx->out, "  %%v%d = add i64 0, %llu\n", else_v, (unsigned long long)CH_VOID);
        }
        if (else_v < 0) {
            return -1;
        }
        fprintf(ctx->out, "  br label %%L%d\n", join_l);
        fprintf(ctx->out, "L%d:\n", join_l);
        int phi = fresh(ctx);
        fprintf(ctx->out, "  %%v%d = phi i64 [ %%v%d, %%L%d ], [ %%v%d, %%L%d ]\n", phi, then_v,
                then_l, else_v, else_l);
        return phi;
    }
    case CH_IR_PRIM_CALL: {
        const char *fn = prim_rt_name(node->as.prim_call.prim);
        if (!fn || node->as.prim_call.arg_count == 0) {
            ctx->ok = false;
            return -1;
        }
        int acc = emit_value(ctx, node->as.prim_call.args[0]);
        if (acc < 0) {
            return -1;
        }
        if (node->as.prim_call.arg_count == 1 && node->as.prim_call.prim == CH_IR_PRIM_SUB) {
            int zero = fresh(ctx);
            fprintf(ctx->out, "  %%v%d = add i64 0, %llu\n", zero,
                    (unsigned long long)ch_make_fixnum(0));
            int id = fresh(ctx);
            fprintf(ctx->out, "  %%v%d = call i64 @%s(i64 %%v%d, i64 %%v%d)\n", id, fn, zero, acc);
            return id;
        }
        if (node->as.prim_call.prim == CH_IR_PRIM_NOT) {
            int cmp = fresh(ctx);
            int id = fresh(ctx);
            fprintf(ctx->out, "  %%c%d = icmp eq i64 %%v%d, %llu\n", cmp, acc,
                    (unsigned long long)CH_FALSE);
            fprintf(ctx->out, "  %%v%d = select i1 %%c%d, i64 %llu, i64 %llu\n", id, cmp,
                    (unsigned long long)CH_TRUE, (unsigned long long)CH_FALSE);
            return id;
        }
        for (size_t i = 1; i < node->as.prim_call.arg_count; i++) {
            int arg = emit_value(ctx, node->as.prim_call.args[i]);
            if (arg < 0) {
                return -1;
            }
            int id = fresh(ctx);
            fprintf(ctx->out, "  %%v%d = call i64 @%s(i64 %%v%d, i64 %%v%d)\n", id, fn, acc, arg);
            acc = id;
        }
        return acc;
    }
    case CH_IR_DEFINE: {
        if (!ch_is_symbol(node->as.define_expr.target) || !node->as.define_expr.value) {
            ctx->ok = false;
            return -1;
        }
        const char *name = ch_symbol_basename(ch_as_symbol(node->as.define_expr.target));
        int val = emit_value(ctx, node->as.define_expr.value);
        if (val < 0 || !name) {
            return -1;
        }
        char san[128];
        sanitize_sym(name, san, sizeof(san));
        int name_id = fresh(ctx);
        fprintf(ctx->out, "  %%n%d = getelementptr inbounds [%zu x i8], ptr @.str_%s, i64 0, i64 0\n",
                name_id, strlen(name) + 1, san);
        fprintf(ctx->out, "  call void @ch_rt_define_global(ptr %%vm, ptr %%n%d, i64 %zu, i64 %%v%d)\n",
                name_id, strlen(name), val);
        return val;
    }
    case CH_IR_AND:
    case CH_IR_OR: {
        ChIrNodeArray arr = node->kind == CH_IR_AND ? node->as.and_expr : node->as.or_expr;
        if (arr.count == 0) {
            int id = fresh(ctx);
            fprintf(ctx->out, "  %%v%d = add i64 0, %llu\n", id,
                    (unsigned long long)(node->kind == CH_IR_AND ? CH_TRUE : CH_FALSE));
            return id;
        }
        int last = -1;
        for (size_t i = 0; i < arr.count; i++) {
            last = emit_value(ctx, arr.items[i]);
            if (last < 0) {
                return -1;
            }
        }
        return last;
    }
    default:
        ctx->ok = false;
        return -1;
    }
}

static void sanitize_sym(const char *name, char *out, size_t out_sz) {
    size_t j = 0;
    for (size_t i = 0; name[i] && j + 1 < out_sz; i++) {
        unsigned char c = (unsigned char)name[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '_' || c == '.') {
            out[j++] = (char)c;
        } else {
            if (j + 3 >= out_sz) {
                break;
            }
            out[j++] = '_';
            static const char hex[] = "0123456789abcdef";
            out[j++] = hex[(c >> 4) & 0xf];
            out[j++] = hex[c & 0xf];
        }
    }
    out[j] = '\0';
    if (j == 0 && out_sz > 1) {
        out[0] = 'x';
        out[1] = '\0';
    }
}

static void emit_string_global(FILE *out, const char *sym, const char *text) {
    size_t len = strlen(text);
    fprintf(out, "@%s = private unnamed_addr constant [%zu x i8] c\"", sym, len + 1);
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)text[i];
        if (c >= 32 && c < 127 && c != '"' && c != '\\') {
            fputc((char)c, out);
        } else {
            fprintf(out, "\\%02X", c);
        }
    }
    fprintf(out, "\\00\"\n");
}

static void emit_source_global(FILE *out, const char *src, size_t len) {
    fprintf(out, "@.ch_src = private unnamed_addr constant [%zu x i8] c\"", len + 1);
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c >= 32 && c < 127 && c != '"' && c != '\\') {
            fputc((char)c, out);
        } else {
            fprintf(out, "\\%02X", c);
        }
    }
    fprintf(out, "\\00\"\n");
}

static void emit_runtime_decls(FILE *out) {
    fprintf(out, "declare ptr @ch_rt_init()\n");
    fprintf(out, "declare void @ch_rt_deinit(ptr)\n");
    fprintf(out, "declare i64 @ch_rt_eval(ptr, ptr, i64)\n");
    fprintf(out, "declare i64 @ch_rt_global_lookup(ptr, ptr, i64)\n");
    fprintf(out, "declare void @ch_rt_define_global(ptr, ptr, i64, i64)\n");
    fprintf(out, "declare i64 @ch_rt_fixnum_add(i64, i64)\n");
    fprintf(out, "declare i64 @ch_rt_fixnum_sub(i64, i64)\n");
    fprintf(out, "declare i64 @ch_rt_fixnum_mul(i64, i64)\n");
    fprintf(out, "declare i64 @ch_rt_fixnum_lt(i64, i64)\n");
    fprintf(out, "declare i64 @ch_rt_fixnum_eq(i64, i64)\n");
    fprintf(out, "declare i32 @ch_rt_main()\n\n");
}

static void collect_define_names(const ChIrNode *node, char ***names, size_t *count, size_t *cap) {
    if (!node) {
        return;
    }
    if (node->kind == CH_IR_DEFINE && ch_is_symbol(node->as.define_expr.target)) {
        const char *name = ch_symbol_basename(ch_as_symbol(node->as.define_expr.target));
        if (name) {
            if (*count >= *cap) {
                *cap = *cap ? *cap * 2 : 8;
                *names = (char **)realloc(*names, *cap * sizeof(char *));
            }
            (*names)[(*count)++] = strdup(name);
        }
        collect_define_names(node->as.define_expr.value, names, count, cap);
        return;
    }
    if (node->kind == CH_IR_SEQ) {
        for (size_t i = 0; i < node->as.seq.count; i++) {
            collect_define_names(node->as.seq.items[i], names, count, cap);
        }
    }
}

static void collect_var_names(const ChIrNode *node, char ***names, size_t *count, size_t *cap) {
    if (!node) {
        return;
    }
    if (node->kind == CH_IR_VAR && node->as.var) {
        const char *name = ch_symbol_basename(node->as.var);
        if (name) {
            for (size_t i = 0; i < *count; i++) {
                if (strcmp((*names)[i], name) == 0) {
                    return;
                }
            }
            if (*count >= *cap) {
                *cap = *cap ? *cap * 2 : 8;
                *names = (char **)realloc(*names, *cap * sizeof(char *));
            }
            (*names)[(*count)++] = strdup(name);
        }
        return;
    }
    switch (node->kind) {
    case CH_IR_IF:
        collect_var_names(node->as.if_expr.test, names, count, cap);
        collect_var_names(node->as.if_expr.consequent, names, count, cap);
        if (node->as.if_expr.has_alternate) {
            collect_var_names(node->as.if_expr.alternate, names, count, cap);
        }
        break;
    case CH_IR_SEQ:
        for (size_t i = 0; i < node->as.seq.count; i++) {
            collect_var_names(node->as.seq.items[i], names, count, cap);
        }
        break;
    case CH_IR_PRIM_CALL:
        for (size_t i = 0; i < node->as.prim_call.arg_count; i++) {
            collect_var_names(node->as.prim_call.args[i], names, count, cap);
        }
        break;
    case CH_IR_DEFINE:
        collect_var_names(node->as.define_expr.value, names, count, cap);
        break;
    case CH_IR_AND:
        for (size_t i = 0; i < node->as.and_expr.count; i++) {
            collect_var_names(node->as.and_expr.items[i], names, count, cap);
        }
        break;
    case CH_IR_OR:
        for (size_t i = 0; i < node->as.or_expr.count; i++) {
            collect_var_names(node->as.or_expr.items[i], names, count, cap);
        }
        break;
    default:
        break;
    }
}

static int lower_file_to_llvm(const char *path, FILE *out) {
    size_t len = 0;
    char *src = ch_read_file(path, &len);
    if (!src) {
        fprintf(stderr, "Error opening file '%s'\n", path);
        return CH_EXIT_ERROR;
    }

    if (source_has_import(src, len)) {
        fprintf(stderr, "compile: refusing sources that use import (native runtime has no lib-path)\n");
        free(src);
        return CH_EXIT_ERROR;
    }

    if (!ch_rt_native_arch_supported()) {
        fprintf(stderr, "compile: native backend supports aarch64/x86_64 only\n");
        free(src);
        return CH_EXIT_ERROR;
    }

    ChVM vm;
    ch_vm_init(&vm);
    ch_vm_register_primitives(&vm);
    vm.script_path = path;

    ChReader reader;
    ch_reader_init(&reader, &vm.gc, src, len);

    int rc = CH_EXIT_OK;
    ChIrNode **forms = NULL;
    size_t form_count = 0;
    size_t form_cap = 0;
    bool all_emittable = true;
    bool last_form_constant = false;
    int64_t last_form_value = 0;

    for (;;) {
        ChValue v = CH_NIL;
        ch_gc_push(&vm.gc, &v);
        ChReadStatus st = ch_read_datum(&reader, &v);
        if (st == CH_READ_EOF) {
            ch_gc_pop(&vm.gc);
            break;
        }
        if (st != CH_READ_OK) {
            fprintf(stderr, "emit-llvm: read error: %s\n", ch_reader_error(&reader));
            ch_gc_pop(&vm.gc);
            rc = CH_EXIT_ERROR;
            break;
        }

        ChValue expanded = CH_NIL;
        ch_gc_push(&vm.gc, &expanded);
        char err[256];
        if (ch_expand_toplevel(&vm, v, &expanded, err, sizeof(err)) != CH_EXPAND_OK) {
            fprintf(stderr, "emit-llvm: expand error: %s\n", err);
            ch_gc_pop_n(&vm.gc, 2);
            rc = CH_EXIT_ERROR;
            break;
        }

        ChCompiler compiler;
        ch_compiler_init(&compiler, &vm);
        ChIrNode *ir = NULL;
        if (ch_ir_lower(&compiler, expanded, &ir) != CH_COMPILE_OK) {
            fprintf(stderr, "emit-llvm: lower error: %s\n", ch_compiler_error(&compiler));
            ch_gc_pop_n(&vm.gc, 2);
            rc = CH_EXIT_ERROR;
            break;
        }
        ch_ir_analyze(ir);
        if (ch_ir_optimize(&compiler, &ir) != CH_COMPILE_OK) {
            fprintf(stderr, "emit-llvm: optimize error: %s\n", ch_compiler_error(&compiler));
            ch_ir_free(ir);
            ch_gc_pop_n(&vm.gc, 2);
            rc = CH_EXIT_ERROR;
            break;
        }

        if (form_count >= form_cap) {
            form_cap = form_cap ? form_cap * 2 : 8;
            forms = (ChIrNode **)realloc(forms, form_cap * sizeof(ChIrNode *));
        }
        forms[form_count++] = ir;
        if (!ch_ir_llvm_emittable(ir)) {
            all_emittable = false;
        }
        int64_t value = 0;
        last_form_constant = ir_eval_fixnum_expr(ir, &value);
        if (last_form_constant) {
            last_form_value = value;
        }
        ch_gc_pop_n(&vm.gc, 2);
    }

    if (rc == CH_EXIT_OK) {
        enum { MODE_CONST, MODE_NATIVE, MODE_EVAL } mode = MODE_EVAL;
        if (last_form_constant && form_count > 0) {
            mode = MODE_CONST;
        } else if (all_emittable && form_count > 0) {
            mode = MODE_NATIVE;
        }

        fprintf(out, "; ModuleID = 'chaaya'\n");
        fprintf(out, "; source = %s\n", path);
        fprintf(out, "; Chaaya %s LLVM backend\n", CHAAYA_VERSION);
        fprintf(out, "target triple = \"%s\"\n\n", host_triple());

        if (mode == MODE_CONST) {
            fprintf(out, "define i32 @main() {\nentry:\n");
            fprintf(out, "  ret i32 %d\n}\n", (int32_t)last_form_value);
        } else if (mode == MODE_NATIVE) {
            char *body = NULL;
            size_t body_sz = 0;
            FILE *mem = open_memstream(&body, &body_sz);
            if (!mem) {
                mode = MODE_EVAL;
            } else {
                emit_runtime_decls(mem);
                char **names = NULL;
                size_t ncount = 0;
                size_t ncap = 0;
                for (size_t i = 0; i < form_count; i++) {
                    collect_define_names(forms[i], &names, &ncount, &ncap);
                    collect_var_names(forms[i], &names, &ncount, &ncap);
                }
                for (size_t i = 0; i < ncount; i++) {
                    char san[128];
                    char sym[160];
                    sanitize_sym(names[i], san, sizeof(san));
                    snprintf(sym, sizeof(sym), ".str_%s", san);
                    emit_string_global(mem, sym, names[i]);
                    snprintf(sym, sizeof(sym), ".name_%s", san);
                    emit_string_global(mem, sym, names[i]);
                }
                fprintf(mem, "\ndefine i32 @main() {\nentry:\n");
                fprintf(mem, "  %%vm = call ptr @ch_rt_init()\n");
                EmitCtx ctx = {.out = mem, .next_id = 0, .ok = true};
                for (size_t i = 0; i < form_count; i++) {
                    if (emit_value(&ctx, forms[i]) < 0 || !ctx.ok) {
                        mode = MODE_EVAL;
                        break;
                    }
                }
                for (size_t i = 0; i < ncount; i++) {
                    free(names[i]);
                }
                free(names);
                if (mode == MODE_NATIVE) {
                    fprintf(mem, "  call void @ch_rt_deinit(ptr %%vm)\n");
                    fprintf(mem, "  ret i32 0\n}\n");
                    fclose(mem);
                    fwrite(body, 1, body_sz, out);
                } else {
                    fclose(mem);
                }
                free(body);
            }
        }

        if (mode == MODE_EVAL) {
            emit_runtime_decls(out);
            emit_source_global(out, src, len);
            fprintf(out, "\ndefine i32 @main() {\nentry:\n");
            fprintf(out, "  %%p = getelementptr inbounds [%zu x i8], ptr @.ch_src, i64 0, i64 0\n",
                    len + 1);
            fprintf(out, "  %%vm = call ptr @ch_rt_init()\n");
            fprintf(out, "  %%_ = call i64 @ch_rt_eval(ptr %%vm, ptr %%p, i64 %zu)\n", len);
            fprintf(out, "  call void @ch_rt_deinit(ptr %%vm)\n");
            fprintf(out, "  ret i32 0\n}\n");
        }
    }

    for (size_t i = 0; i < form_count; i++) {
        ch_ir_free(forms[i]);
    }
    free(forms);
    free(src);
    ch_vm_deinit(&vm);
    return rc;
}

int ch_llvm_backend_emit_ir(const char *path, const char *out_path) {
    FILE *out = stdout;
    int close_out = 0;
    if (out_path) {
        out = fopen(out_path, "w");
        if (!out) {
            fprintf(stderr, "emit-llvm: cannot write '%s'\n", out_path);
            return CH_EXIT_ERROR;
        }
        close_out = 1;
    }
    int rc = lower_file_to_llvm(path, out);
    if (close_out) {
        fclose(out);
        if (rc == CH_EXIT_OK) {
            fprintf(stderr, "emit-llvm: wrote %s\n", out_path);
        }
    }
    return rc;
}

static int run_command_exit_code(const char *cmd) {
    int st = system(cmd);
    if (st == -1) {
        return -1;
    }
    if (WIFEXITED(st)) {
        return WEXITSTATUS(st);
    }
    return -1;
}

static int path_has_executable(const char *name) {
    if (!name || !name[0]) {
        return 0;
    }
    if (strchr(name, '/')) {
        return access(name, X_OK) == 0;
    }
    const char *path_env = getenv("PATH");
    if (!path_env || !path_env[0]) {
        return 0;
    }
    char *copy = strdup(path_env);
    if (!copy) {
        return 0;
    }
    int found = 0;
    char *save = NULL;
    for (char *dir = strtok_r(copy, ":", &save); dir; dir = strtok_r(NULL, ":", &save)) {
        if (!dir[0]) {
            continue;
        }
        char probe[PATH_MAX];
        if (snprintf(probe, sizeof(probe), "%s/%s", dir, name) >= (int)sizeof(probe)) {
            continue;
        }
        if (access(probe, X_OK) == 0) {
            found = 1;
            break;
        }
    }
    free(copy);
    return found;
}

static int add_unique_compiler(const char **list, size_t *count, size_t cap, const char *compiler) {
    if (!compiler || !compiler[0]) {
        return 0;
    }
    for (size_t i = 0; i < *count; i++) {
        if (strcmp(list[i], compiler) == 0) {
            return 0;
        }
    }
    if (*count >= cap) {
        return -1;
    }
    list[(*count)++] = compiler;
    return 0;
}

static int find_libchaaya_rt(char *out, size_t out_sz) {
    const char *candidates[8];
    size_t n = 0;
    const char *env = getenv("CHAAYA_LIB_DIR");
    static char env_path[PATH_MAX];
    static char src_path[PATH_MAX];
    static char build_path[PATH_MAX];
    if (env && env[0]) {
        snprintf(env_path, sizeof(env_path), "%s/libchaaya_rt.a", env);
        candidates[n++] = env_path;
    }
    snprintf(build_path, sizeof(build_path), "%s/libchaaya_rt.a", CHAAYA_BUILD_DIR);
    candidates[n++] = build_path;
    snprintf(src_path, sizeof(src_path), "%s/build/libchaaya_rt.a", CHAAYA_SOURCE_DIR);
    candidates[n++] = src_path;
    candidates[n++] = "libchaaya_rt.a";
    candidates[n++] = "./libchaaya_rt.a";

    for (size_t i = 0; i < n; i++) {
        if (access(candidates[i], R_OK) == 0) {
            snprintf(out, out_sz, "%s", candidates[i]);
            return 0;
        }
    }
    return -1;
}

static int ll_needs_runtime(const char *ll_path) {
    FILE *f = fopen(ll_path, "r");
    if (!f) {
        return 1;
    }
    char line[512];
    int needs = 0;
    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, "@ch_rt_")) {
            needs = 1;
            break;
        }
    }
    fclose(f);
    return needs;
}

static int try_link_native(const char *compiler, const char *ll_path, const char *rt_lib,
                           const char *bin, int needs_rt) {
    char cmd[4096];
    if (!needs_rt) {
        if (snprintf(cmd, sizeof(cmd), "%s -O2 -Wno-override-module -o \"%s\" \"%s\"", compiler, bin,
                     ll_path) >= (int)sizeof(cmd)) {
            return -1;
        }
        return run_command_exit_code(cmd) == 0 ? 0 : -1;
    }
    if (!rt_lib) {
        return -1;
    }
#if defined(__APPLE__)
    const char *extra = "";
#else
    const char *extra = "-ldl -lpthread -lm";
#endif
    if (snprintf(cmd, sizeof(cmd),
                 "%s -O2 -Wno-override-module -o \"%s\" \"%s\" \"%s\" %s", compiler, bin, ll_path,
                 rt_lib, extra) >= (int)sizeof(cmd)) {
        return -1;
    }
    return run_command_exit_code(cmd) == 0 ? 0 : -1;
}

int ch_llvm_backend_compile_native(const char *path, const char *out_path) {
    const char *bin = out_path ? out_path : "a.out";
    char ll_path[PATH_MAX];
    if (snprintf(ll_path, sizeof(ll_path), "/tmp/chaaya-%d.ll", (int)getpid()) >= (int)sizeof(ll_path)) {
        fprintf(stderr, "compile: temp path too long\n");
        return CH_EXIT_ERROR;
    }

    int rc = ch_llvm_backend_emit_ir(path, ll_path);
    if (rc != CH_EXIT_OK) {
        unlink(ll_path);
        return rc;
    }

    int needs_rt = ll_needs_runtime(ll_path);
    char rt_lib[PATH_MAX];
    const char *rt_lib_ptr = NULL;
    if (needs_rt) {
        if (find_libchaaya_rt(rt_lib, sizeof(rt_lib)) != 0) {
            fprintf(stderr,
                    "compile: emitted LLVM IR to %s but libchaaya_rt.a was not found. "
                    "Set CHAAYA_LIB_DIR or build chaaya_rt.\n",
                    ll_path);
            return CH_EXIT_ERROR;
        }
        rt_lib_ptr = rt_lib;
    }

    const char *compilers[5];
    size_t compiler_count = 0;
    const char *env_compiler = getenv("CHAAYA_LLVM_CC");
    (void)add_unique_compiler(compilers, &compiler_count, 5, env_compiler);
    if (path_has_executable("clang")) {
        (void)add_unique_compiler(compilers, &compiler_count, 5, "clang");
    }
    if (path_has_executable("zig")) {
        /* zig cc can compile .ll */
    }
    if (path_has_executable("cc")) {
        (void)add_unique_compiler(compilers, &compiler_count, 5, "cc");
    }
    if (compiler_count == 0) {
        compilers[compiler_count++] = "cc";
    }

    for (size_t i = 0; i < compiler_count; i++) {
        if (try_link_native(compilers[i], ll_path, rt_lib_ptr, bin, needs_rt) == 0) {
            fprintf(stderr, "compile: wrote %s (llvm ir at %s, cc=%s%s%s)\n", bin, ll_path,
                    compilers[i], needs_rt ? ", rt=" : "", needs_rt ? rt_lib : "");
            return CH_EXIT_OK;
        }
    }

    fprintf(stderr,
            "compile: emitted LLVM IR to %s but failed to link native binary. "
            "Try setting CHAAYA_LLVM_CC=clang.\n",
            ll_path);
    return CH_EXIT_ERROR;
}

int ch_llvm_backend_run_file(const char *path) {
    if (!path) {
        path = "<stdin>";
    }
    char bin[PATH_MAX];
    if (snprintf(bin, sizeof(bin), "/tmp/chaaya-run-%d", (int)getpid()) >= (int)sizeof(bin)) {
        return CH_EXIT_ERROR;
    }
    int rc = ch_llvm_backend_compile_native(path, bin);
    if (rc != CH_EXIT_OK) {
        return rc;
    }
    char cmd[PATH_MAX + 4];
    if (snprintf(cmd, sizeof(cmd), "\"%s\"", bin) >= (int)sizeof(cmd)) {
        unlink(bin);
        return CH_EXIT_ERROR;
    }
    int st = run_command_exit_code(cmd);
    unlink(bin);
    /* Propagate the native program's exit code (constant-exit MVP uses it). */
    if (st < 0) {
        return CH_EXIT_ERROR;
    }
    return st;
}
