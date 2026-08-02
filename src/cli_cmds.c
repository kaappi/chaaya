#include "chaaya/cli.h"
#include "cli_internal.h"

#include "chaaya/compiler.h"
#include "chaaya/diagnostics.h"
#include "chaaya/eval.h"
#include "chaaya/expander.h"
#include "chaaya/features.h"
#include "chaaya/fmt.h"
#include "chaaya/ir.h"
#include "chaaya/printer.h"
#include "chaaya/reader.h"
#include "chaaya/test_runner.h"
#include "chaaya/value.h"

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

int not_implemented(const char *what) {
    fprintf(stderr, "chaaya: '%s' is not implemented yet (bootstrap)\n", what);
    return CH_EXIT_ERROR;
}

int cmd_test(const char *argv0, ChCliOptions *opts) {
    if (!opts->test_since) {
        opts->test_since = "HEAD";
    }
    return ch_test_run(argv0, opts);
}

int cmd_wasm_stub(void) {
    char script_path[PATH_MAX];
    if (snprintf(script_path, sizeof(script_path), "%s/scripts/build-wasm.sh", CHAAYA_SOURCE_DIR) >=
        (int)sizeof(script_path)) {
        fprintf(stderr, "chaaya: wasm script path is too long\n");
        return CH_EXIT_ERROR;
    }

    if (access(script_path, F_OK) == 0) {
        char cmd[PATH_MAX + 32];
        if (access(script_path, X_OK) == 0) {
            if (snprintf(cmd, sizeof(cmd), "\"%s\"", script_path) >= (int)sizeof(cmd)) {
                fprintf(stderr, "chaaya: wasm command line is too long\n");
                return CH_EXIT_ERROR;
            }
        } else if (access(script_path, R_OK) == 0) {
            if (snprintf(cmd, sizeof(cmd), "bash \"%s\"", script_path) >= (int)sizeof(cmd)) {
                fprintf(stderr, "chaaya: wasm command line is too long\n");
                return CH_EXIT_ERROR;
            }
        } else {
            fprintf(stderr, "chaaya: wasm helper exists but is not readable: %s\n", script_path);
            fprintf(stderr, "Manual build:\n");
            fprintf(stderr,
                    "  cmake -S . -B build-wasm -DCHAAYA_WASM=ON "
                    "-DCMAKE_TOOLCHAIN_FILE=<wasi-sdk.cmake>\n");
            fprintf(stderr, "  cmake --build build-wasm -j --target chaaya-wasm\n");
            return CH_EXIT_OK;
        }

        fprintf(stderr, "chaaya: running wasm build helper: %s\n", script_path);
        int st = system(cmd);
        if (st == -1 || !WIFEXITED(st) || WEXITSTATUS(st) != 0) {
            int code = (st != -1 && WIFEXITED(st)) ? WEXITSTATUS(st) : -1;
            fprintf(stderr, "chaaya: wasm build helper failed (exit=%d)\n", code);
            fprintf(stderr, "Manual build:\n");
            fprintf(stderr,
                    "  cmake -S . -B build-wasm -DCHAAYA_WASM=ON "
                    "-DCMAKE_TOOLCHAIN_FILE=<wasi-sdk.cmake>\n");
            fprintf(stderr, "  cmake --build build-wasm -j --target chaaya-wasm\n");
            return CH_EXIT_ERROR;
        }
        return CH_EXIT_OK;
    }

    fprintf(stderr, "chaaya: wasm helper not found at %s\n", script_path);
    fprintf(stderr, "Manual build:\n");
    fprintf(stderr,
            "  cmake -S . -B build-wasm -DCHAAYA_WASM=ON "
            "-DCMAKE_TOOLCHAIN_FILE=<wasi-sdk.cmake>\n");
    fprintf(stderr, "  cmake --build build-wasm -j --target chaaya-wasm\n");
    return CH_EXIT_OK;
}


int cmd_features(ChCliOptions *opts) {
    if (opts->json) {
        return ch_features_print_json(stdout, opts->lib_paths, opts->lib_path_count);
    }
    ch_features_print_text(stdout);
    return CH_EXIT_OK;
}

ChReadStatus read_cli_datum(ChVM *vm, ChReader *reader, ChValue *out) {
    /* Globals/macros/libraries are marked during GC; no sticky root flood. */
    (void)vm;
    return ch_read_datum(reader, out);
}

int cmd_ast(const char *path) {
    size_t len = 0;
    char *src = ch_read_file(path, &len);
    if (!src) {
        fprintf(stderr, "Error opening file '%s'\n", path);
        return CH_EXIT_ERROR;
    }
    ChVM vm;
    ch_vm_init(&vm);
    ChReader reader;
    ch_reader_init(&reader, &vm.gc, src, len);
    for (;;) {
        ChValue v = CH_NIL;
        ch_gc_push(&vm.gc, &v);
        ChReadStatus st = read_cli_datum(&vm, &reader, &v);
        if (st == CH_READ_EOF) {
            ch_gc_pop(&vm.gc);
            break;
        }
        if (st != CH_READ_OK) {
            fprintf(stderr, "read error: %s\n", ch_reader_error(&reader));
            ch_gc_pop(&vm.gc);
            free(src);
            ch_vm_deinit(&vm);
            return CH_EXIT_ERROR;
        }
        ch_print_value(stdout, v, false);
        fputc('\n', stdout);
        ch_gc_pop(&vm.gc);
    }
    free(src);
    ch_vm_deinit(&vm);
    return CH_EXIT_OK;
}

int cmd_expand(const char *path, const ChCliOptions *opts) {
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
    for (size_t i = 0; i < opts->lib_path_count; i++) {
        vm.lib_paths[vm.lib_path_count++] = opts->lib_paths[i];
    }
    ChReader reader;
    ch_reader_init(&reader, &vm.gc, src, len);
    for (;;) {
        ChValue v = CH_NIL;
        ch_gc_push(&vm.gc, &v);
        ChReadStatus st = read_cli_datum(&vm, &reader, &v);
        if (st == CH_READ_EOF) {
            ch_gc_pop(&vm.gc);
            break;
        }
        if (st != CH_READ_OK) {
            fprintf(stderr, "read error: %s\n", ch_reader_error(&reader));
            ch_gc_pop(&vm.gc);
            free(src);
            ch_vm_deinit(&vm);
            return CH_EXIT_ERROR;
        }
        ChValue out = CH_NIL;
        ch_gc_push(&vm.gc, &out);
        char err[256];
        if (ch_expand_toplevel(&vm, v, &out, err, sizeof(err)) != CH_EXPAND_OK) {
            fprintf(stderr, "expand error: %s\n", err);
            ch_gc_pop_n(&vm.gc, 2);
            free(src);
            ch_vm_deinit(&vm);
            return CH_EXIT_ERROR;
        }
        if (out != CH_VOID) {
            ch_print_value(stdout, out, false);
            fputc('\n', stdout);
        }
        ch_gc_pop_n(&vm.gc, 2);
    }
    free(src);
    ch_vm_deinit(&vm);
    return CH_EXIT_OK;
}

int cmd_fmt(const char *path, const ChCliOptions *opts) {
    return ch_fmt_file(path, opts->flag_fmt_check, opts->output);
}

void report_reader_diag(const char *path, const char *src, size_t len, const ChReader *reader) {
    ChDiagCode code = ch_reader_error_code(reader);
    ch_diag_report_read(stderr, path, src, len, reader->pos, code, ch_reader_error(reader));
}

void report_compiler_diag(const char *path, const ChCompiler *compiler) {
    ChDiagCode code = ch_compiler_error_code(compiler);
    ch_diag_report_simple(stderr, path, compiler->error_line, compiler->error_column, code, NULL,
                          ch_compiler_error(compiler));
}

static int is_special_symbol(const char *name) {
    static const char *const specials[] = {
        "quote",    "if",         "lambda",  "define", "set!",     "begin",
        "and",      "or",         "let",     "let*",   "letrec",   "letrec*",
        "cond",     "case",       "do",      "delay",  "delay-force", "quasiquote",
        "unquote",  "unquote-splicing", "define-syntax", "let-syntax", "letrec-syntax",
        "syntax-rules", "include", "include-ci", "cond-expand", "import", "define-library",
        "define-values", "let-values", "let*-values", "case-lambda", "parameterize",
        "guard", "when", "unless", "else", "=>", "...", "_", NULL};
    for (int i = 0; specials[i]; i++) {
        if (strcmp(name, specials[i]) == 0) {
            return 1;
        }
    }
    return 0;
}

static int global_is_defined(ChVM *vm, const char *name) {
    for (size_t i = 0; i < vm->global_count; i++) {
        if (vm->globals[i].defined && strcmp(vm->globals[i].name->name, name) == 0) {
            return 1;
        }
    }
    for (size_t i = 0; i < vm->macro_count; i++) {
        if (vm->macros[i].name && strcmp(vm->macros[i].name->name, name) == 0) {
            return 1;
        }
    }
    return 0;
}

static ChNative *find_native(ChVM *vm, const char *name) {
    for (size_t i = 0; i < vm->global_count; i++) {
        if (!vm->globals[i].defined) {
            continue;
        }
        if (strcmp(vm->globals[i].name->name, name) != 0) {
            continue;
        }
        if (ch_is_native(vm->globals[i].value)) {
            return ch_as_native(vm->globals[i].value);
        }
    }
    return NULL;
}

static void lint_walk(ChVM *vm, ChValue expr, const char *path, int *warns, int *errors,
                      int deny_warnings) {
    if (ch_is_symbol(expr)) {
        const char *name = ch_as_symbol(expr)->name;
        if (!is_special_symbol(name) && !global_is_defined(vm, name)) {
            char msg[256];
            snprintf(msg, sizeof(msg), "unknown top-level variable '%s'", name);
            ch_diag_report_simple(stderr, path, 0, 0, CH_DIAG_UNKNOWN_TOPLEVEL_VARIABLE, NULL, msg);
            (*warns)++;
            if (deny_warnings) {
                (*errors)++;
            }
        }
        return;
    }
    if (!ch_is_pair(expr)) {
        return;
    }
    ChValue head = ch_car(expr);
    if (ch_is_symbol(head)) {
        const char *name = ch_as_symbol(head)->name;
        ChNative *nat = find_native(vm, name);
        if (nat) {
            int nargs = 0;
            for (ChValue a = ch_cdr(expr); ch_is_pair(a); a = ch_cdr(a)) {
                nargs++;
            }
            int bad = 0;
            if (nat->arity >= 0) {
                bad = nargs != nat->arity;
            } else {
                bad = nargs < nat->min_arity;
            }
            if (bad) {
                char msg[256];
                snprintf(msg, sizeof(msg), "primitive '%s' arity mismatch (got %d)", name, nargs);
                ch_diag_report_simple(stderr, path, 0, 0, CH_DIAG_PRIMITIVE_ARITY_MISMATCH, NULL,
                                      msg);
                (*errors)++;
            }
        }
    }
    ChValue p = expr;
    for (; ch_is_pair(p); p = ch_cdr(p)) {
        lint_walk(vm, ch_car(p), path, warns, errors, deny_warnings);
    }
    if (!ch_is_nil(p)) {
        lint_walk(vm, p, path, warns, errors, deny_warnings);
    }
}

int cmd_check(const char *path, const ChCliOptions *opts) {
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
    for (size_t i = 0; i < opts->lib_path_count; i++) {
        vm.lib_paths[vm.lib_path_count++] = opts->lib_paths[i];
    }

    ChReader reader;
    ch_reader_init(&reader, &vm.gc, src, len);

    size_t forms = 0;
    int warns = 0;
    int errors = 0;
    int rc = CH_EXIT_OK;
    for (;;) {
        ChValue v = CH_NIL;
        ch_gc_push(&vm.gc, &v);
        size_t form_start = reader.pos;
        int form_line = 0;
        int form_col = 0;
        ch_diag_location_from_offset(src, len, form_start, &form_line, &form_col);
        ChReadStatus st = read_cli_datum(&vm, &reader, &v);
        if (st == CH_READ_EOF) {
            ch_gc_pop(&vm.gc);
            break;
        }
        if (st != CH_READ_OK) {
            report_reader_diag(path, src, len, &reader);
            ch_gc_pop(&vm.gc);
            rc = CH_EXIT_ERROR;
            break;
        }

        ChValue expanded = CH_NIL;
        ch_gc_push(&vm.gc, &expanded);
        char err[256];
        if (ch_expand_toplevel(&vm, v, &expanded, err, sizeof(err)) != CH_EXPAND_OK) {
            ChDiagCode code = ch_diag_classify_message(err, CH_DIAG_STAGE_COMPILE);
            ch_diag_report_simple(stderr, path, form_line, form_col, code, NULL, err);
            ch_gc_pop_n(&vm.gc, 2);
            rc = CH_EXIT_ERROR;
            break;
        }

        lint_walk(&vm, expanded, path, &warns, &errors, opts->flag_deny_warnings);

        ChCompiler compiler;
        ch_compiler_init(&compiler, &vm);
        ch_compiler_set_location(&compiler, form_line, form_col);
        ChFunction *fn = NULL;
        if (ch_compile_toplevel(&compiler, expanded, &fn) != CH_COMPILE_OK) {
            report_compiler_diag(path, &compiler);
            ch_gc_pop_n(&vm.gc, 2);
            rc = CH_EXIT_ERROR;
            break;
        }
        (void)fn;
        forms++;
        ch_gc_pop_n(&vm.gc, 2);
    }

    if (errors > 0) {
        rc = CH_EXIT_ERROR;
    }
    if (rc == CH_EXIT_OK) {
        if (warns > 0) {
            printf("check: ok with %d warning%s (%zu form%s)\n", warns, warns == 1 ? "" : "s", forms,
                   forms == 1 ? "" : "s");
        } else {
            printf("check: ok (%zu form%s)\n", forms, forms == 1 ? "" : "s");
        }
    }

    free(src);
    ch_vm_deinit(&vm);
    return rc;
}

int cmd_ir(const char *path, const ChCliOptions *opts) {
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
    for (size_t i = 0; i < opts->lib_path_count; i++) {
        vm.lib_paths[vm.lib_path_count++] = opts->lib_paths[i];
    }

    ChReader reader;
    ch_reader_init(&reader, &vm.gc, src, len);
    int rc = CH_EXIT_OK;
    for (;;) {
        ChValue v = CH_NIL;
        ch_gc_push(&vm.gc, &v);
        ChReadStatus st = read_cli_datum(&vm, &reader, &v);
        if (st == CH_READ_EOF) {
            ch_gc_pop(&vm.gc);
            break;
        }
        if (st != CH_READ_OK) {
            report_reader_diag(path, src, len, &reader);
            ch_gc_pop(&vm.gc);
            rc = CH_EXIT_ERROR;
            break;
        }

        ChValue expanded = CH_NIL;
        ch_gc_push(&vm.gc, &expanded);
        char err[256];
        if (ch_expand_toplevel(&vm, v, &expanded, err, sizeof(err)) != CH_EXPAND_OK) {
            ChDiagCode code = ch_diag_classify_message(err, CH_DIAG_STAGE_COMPILE);
            ch_diag_report_simple(stderr, path, 0, 0, code, NULL, err);
            ch_gc_pop_n(&vm.gc, 2);
            rc = CH_EXIT_ERROR;
            break;
        }

        ChCompiler compiler;
        ch_compiler_init(&compiler, &vm);
        ChIrNode *ir = NULL;
        if (ch_ir_lower(&compiler, expanded, &ir) != CH_COMPILE_OK) {
            report_compiler_diag(path, &compiler);
            ch_gc_pop_n(&vm.gc, 2);
            rc = CH_EXIT_ERROR;
            break;
        }
        ch_ir_analyze(ir);
        if (!opts->flag_no_ir_opt) {
            if (ch_ir_optimize(&compiler, &ir) != CH_COMPILE_OK) {
                report_compiler_diag(path, &compiler);
                ch_ir_free(ir);
                ch_gc_pop_n(&vm.gc, 2);
                rc = CH_EXIT_ERROR;
                break;
            }
        }
        ch_ir_print(stdout, ir, 0);
        fputc('\n', stdout);
        ch_ir_free(ir);
        ch_gc_pop_n(&vm.gc, 2);
    }

    free(src);
    ch_vm_deinit(&vm);
    return rc;
}
