#include "chaaya/llvm_backend.h"

#include "chaaya/cli.h"
#include "chaaya/compiler.h"
#include "chaaya/eval.h"
#include "chaaya/expander.h"
#include "chaaya/ir.h"
#include "chaaya/reader.h"
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

static const char *value_symbol_name(ChValue value) {
    if (!ch_is_symbol(value)) {
        return NULL;
    }
    return ch_symbol_basename(ch_as_symbol(value));
}

static bool list_last_item(ChValue list, ChValue *out) {
    if (!ch_is_pair(list) || !out) {
        return false;
    }
    ChValue it = list;
    ChValue last = CH_UNDEFINED;
    while (ch_is_pair(it)) {
        last = ch_car(it);
        it = ch_cdr(it);
    }
    if (!ch_is_nil(it) || last == CH_UNDEFINED) {
        return false;
    }
    *out = last;
    return true;
}

static bool datum_eval_fixnum_expr(ChValue expr, int64_t *out) {
    if (!out) {
        return false;
    }
    if (ch_is_fixnum(expr)) {
        *out = ch_to_fixnum(expr);
        return true;
    }
    if (!ch_is_pair(expr)) {
        return false;
    }
    ChValue op = ch_car(expr);
    const char *name = value_symbol_name(op);
    if (!name) {
        return false;
    }
    ChValue args = ch_cdr(expr);
    if (strcmp(name, "begin") == 0) {
        ChValue last = CH_UNDEFINED;
        return list_last_item(args, &last) && datum_eval_fixnum_expr(last, out);
    }
    if (strcmp(name, "+") == 0) {
        int64_t acc = 0;
        ChValue it = args;
        while (ch_is_pair(it)) {
            int64_t part = 0;
            if (!datum_eval_fixnum_expr(ch_car(it), &part) || !checked_add_i64(acc, part, &acc)) {
                return false;
            }
            it = ch_cdr(it);
        }
        if (!ch_is_nil(it)) {
            return false;
        }
        *out = acc;
        return true;
    }
    if (strcmp(name, "-") == 0) {
        if (!ch_is_pair(args)) {
            return false;
        }
        int64_t acc = 0;
        if (!datum_eval_fixnum_expr(ch_car(args), &acc)) {
            return false;
        }
        ChValue rest = ch_cdr(args);
        if (ch_is_nil(rest)) {
            return checked_sub_i64(0, acc, out);
        }
        while (ch_is_pair(rest)) {
            int64_t part = 0;
            if (!datum_eval_fixnum_expr(ch_car(rest), &part) || !checked_sub_i64(acc, part, &acc)) {
                return false;
            }
            rest = ch_cdr(rest);
        }
        if (!ch_is_nil(rest)) {
            return false;
        }
        *out = acc;
        return true;
    }
    if (strcmp(name, "*") == 0) {
        int64_t acc = 1;
        ChValue it = args;
        while (ch_is_pair(it)) {
            int64_t part = 0;
            if (!datum_eval_fixnum_expr(ch_car(it), &part) || !checked_mul_i64(acc, part, &acc)) {
                return false;
            }
            it = ch_cdr(it);
        }
        if (!ch_is_nil(it)) {
            return false;
        }
        *out = acc;
        return true;
    }
    return false;
}

static bool expanded_extract_main_constant(ChValue expanded, int64_t *out) {
    if (!ch_is_pair(expanded) || !out) {
        return false;
    }
    ChValue head = ch_car(expanded);
    const char *head_name = value_symbol_name(head);
    if (!head_name || strcmp(head_name, "define") != 0) {
        return false;
    }
    ChValue rest = ch_cdr(expanded);
    if (!ch_is_pair(rest)) {
        return false;
    }

    ChValue target = ch_car(rest);
    ChValue body = ch_cdr(rest);

    if (ch_is_symbol(target)) {
        const char *name = value_symbol_name(target);
        if (!name || strcmp(name, "main") != 0 || !ch_is_pair(body)) {
            return false;
        }
        ChValue rhs = ch_car(body);
        if (datum_eval_fixnum_expr(rhs, out)) {
            return true;
        }
        if (ch_is_pair(rhs) && value_symbol_name(ch_car(rhs)) &&
            strcmp(value_symbol_name(ch_car(rhs)), "lambda") == 0) {
            ChValue lambda_rest = ch_cdr(rhs);
            if (!ch_is_pair(lambda_rest)) {
                return false;
            }
            ChValue params = ch_car(lambda_rest);
            ChValue lambda_body = ch_cdr(lambda_rest);
            ChValue last = CH_UNDEFINED;
            if (ch_is_nil(params) && list_last_item(lambda_body, &last)) {
                return datum_eval_fixnum_expr(last, out);
            }
        }
        return false;
    }

    if (!ch_is_pair(target)) {
        return false;
    }
    ChValue fn_name = ch_car(target);
    const char *name = value_symbol_name(fn_name);
    if (!name || strcmp(name, "main") != 0) {
        return false;
    }
    ChValue params = ch_cdr(target);
    if (!ch_is_nil(params)) {
        return false;
    }
    ChValue last = CH_UNDEFINED;
    return list_last_item(body, &last) && datum_eval_fixnum_expr(last, out);
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

static bool ir_extract_main_constant(const ChIrNode *node, int64_t *out) {
    if (!node || !out) {
        return false;
    }
    switch (node->kind) {
    case CH_IR_DEFINE: {
        ChValue target = node->as.define_expr.target;
        if (!ch_is_symbol(target)) {
            return false;
        }
        const char *name = ch_symbol_basename(ch_as_symbol(target));
        if (!name || strcmp(name, "main") != 0) {
            return false;
        }
        ChIrNode *value = node->as.define_expr.value;
        if (!value) {
            return false;
        }
        if (ir_eval_fixnum_expr(value, out)) {
            return true;
        }
        if (value->kind == CH_IR_LAMBDA && ch_is_nil(value->as.lambda.params) &&
            value->as.lambda.body_count > 0) {
            return ir_eval_fixnum_expr(value->as.lambda.body[value->as.lambda.body_count - 1], out);
        }
        return false;
    }
    case CH_IR_SEQ: {
        bool found = false;
        int64_t last = 0;
        for (size_t i = 0; i < node->as.seq.count; i++) {
            int64_t candidate = 0;
            if (ir_extract_main_constant(node->as.seq.items[i], &candidate)) {
                found = true;
                last = candidate;
            }
        }
        if (found) {
            *out = last;
        }
        return found;
    }
    default:
        return false;
    }
}

static void emit_llvm_module(FILE *out, const char *path, bool has_const_exit, int64_t const_exit,
                             size_t form_count) {
    fprintf(out, "; ModuleID = 'chaaya-mvp'\n");
    fprintf(out, "; source = %s\n", path);
    fprintf(out, "; forms = %zu\n", form_count);
    fprintf(out, "; Chaaya %s LLVM MVP lowering\n", CHAAYA_VERSION);
    fprintf(out, "declare i32 @ch_rt_main()\n\n");
    fprintf(out, "define i32 @main() {\n");
    fprintf(out, "entry:\n");
    if (has_const_exit) {
        int32_t exit_code = (int32_t)const_exit;
        if ((int64_t)exit_code != const_exit) {
            fprintf(out, "  ; fixnum %lld truncated to i32\n", (long long)const_exit);
        }
        fprintf(out, "  ret i32 %d\n", exit_code);
    } else {
        fprintf(out, "  %%code = call i32 @ch_rt_main()\n");
        fprintf(out, "  ret i32 %%code\n");
    }
    fprintf(out, "}\n");
}

static int lower_file_to_llvm(const char *path, FILE *out) {
    size_t len = 0;
    char *src = ch_read_file(path, &len);
    if (!src) {
        fprintf(stderr, "Error opening file '%s'\n", path);
        return CH_EXIT_ERROR;
    }

    ChVM vm;
    ch_vm_init(&vm);
    ch_vm_register_primitives(&vm);
    vm.script_path = path;

    ChReader reader;
    ch_reader_init(&reader, &vm.gc, src, len);

    int rc = CH_EXIT_OK;
    size_t form_count = 0;
    bool last_form_constant = false;
    int64_t last_form_value = 0;
    bool main_constant = false;
    int64_t main_value = 0;

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

        form_count++;
        int64_t value = 0;
        bool expanded_constant = datum_eval_fixnum_expr(expanded, &value);
        last_form_constant = expanded_constant || ir_eval_fixnum_expr(ir, &value);
        if (last_form_constant) {
            last_form_value = value;
        }
        if (expanded_extract_main_constant(expanded, &value)) {
            main_constant = true;
            main_value = value;
        }
        if (ir_extract_main_constant(ir, &value)) {
            main_constant = true;
            main_value = value;
        }
        ch_ir_free(ir);
        ch_gc_pop_n(&vm.gc, 2);
    }

    if (rc == CH_EXIT_OK) {
        bool has_const_exit = last_form_constant || main_constant;
        int64_t const_exit = last_form_constant ? last_form_value : main_value;
        emit_llvm_module(out, path, has_const_exit, const_exit, form_count);
    }

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

static int try_link_native(const char *compiler, const char *ll_path, const char *rt_source,
                           const char *bin) {
    char cmd[2048];
    if (snprintf(cmd, sizeof(cmd), "%s -O0 -x ir \"%s\" -x c \"%s\" -o \"%s\"", compiler, ll_path,
                 rt_source, bin) >= (int)sizeof(cmd)) {
        return -1;
    }
    if (run_command_exit_code(cmd) == 0) {
        return 0;
    }
    if (snprintf(cmd, sizeof(cmd), "%s -O0 -o \"%s\" \"%s\" \"%s\"", compiler, bin, ll_path, rt_source) >=
        (int)sizeof(cmd)) {
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

    const char *runtime_source = CHAAYA_SOURCE_DIR "/src/ch_rt_main.c";
    if (access(runtime_source, R_OK) != 0) {
        fprintf(stderr, "compile: runtime source missing at %s\n", runtime_source);
        return CH_EXIT_ERROR;
    }

    const char *compilers[4];
    size_t compiler_count = 0;
    const char *env_compiler = getenv("CHAAYA_LLVM_CC");
    (void)add_unique_compiler(compilers, &compiler_count, 4, env_compiler);
    if (path_has_executable("clang")) {
        (void)add_unique_compiler(compilers, &compiler_count, 4, "clang");
    }
    if (path_has_executable("cc")) {
        (void)add_unique_compiler(compilers, &compiler_count, 4, "cc");
    }
    if (compiler_count == 0) {
        compilers[compiler_count++] = "cc";
    }

    for (size_t i = 0; i < compiler_count; i++) {
        if (try_link_native(compilers[i], ll_path, runtime_source, bin) == 0) {
            fprintf(stderr, "compile: wrote %s (llvm ir at %s, runtime=%s, cc=%s)\n", bin, ll_path,
                    runtime_source, compilers[i]);
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
    return st == 0 ? CH_EXIT_OK : CH_EXIT_ERROR;
}
