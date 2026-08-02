#include "chaaya/cli.h"
#include "cli_internal.h"

#include "chaaya/cache.h"
#include "chaaya/compiler.h"
#include "chaaya/coverage.h"
#include "chaaya/diagnostics.h"
#include "chaaya/disasm.h"
#include "chaaya/doctor.h"
#include "chaaya/eval.h"
#include "chaaya/expander.h"
#include "chaaya/ffi.h"
#include "chaaya/llvm_backend.h"
#include "chaaya/lsp.h"
#include "chaaya/profile.h"
#include "chaaya/reader.h"
#include "chaaya/repl.h"
#include "chaaya/runtime_exports.h"
#include "chaaya/sandbox.h"
#include "chaaya/test_runner.h"
#include "chaaya/timings.h"
#include "chaaya/value.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <unistd.h>

#ifndef CHAAYA_SOURCE_DIR
#define CHAAYA_SOURCE_DIR "."
#endif

static void apply_opts_to_vm(ChVM *vm, ChCliOptions *opts);

static volatile sig_atomic_t g_cli_timeout_fired = 0;

static void cli_timeout_handler(int sig) {
    (void)sig;
    g_cli_timeout_fired = 1;
}

typedef struct ChCompiledTopLevels {
    ChFunction **fns;
    size_t count;
    size_t cap;
} ChCompiledTopLevels;

static int append_compiled_top_level(ChCompiledTopLevels *compiled, ChFunction *fn) {
    if (compiled->count >= compiled->cap) {
        size_t next_cap = compiled->cap ? compiled->cap * 2 : 8;
        ChFunction **next =
            (ChFunction **)realloc(compiled->fns, next_cap * sizeof(ChFunction *));
        if (!next) {
            return -1;
        }
        compiled->fns = next;
        compiled->cap = next_cap;
    }
    compiled->fns[compiled->count++] = fn;
    return 0;
}

/* Returns with root_list pushed on vm->gc; caller must ch_gc_pop once. */
static int compile_source_top_levels(ChVM *vm, const char *path, const char *src, size_t len,
                                     ChCompiledTopLevels *compiled, ChValue *root_list) {
    memset(compiled, 0, sizeof(*compiled));
    *root_list = CH_NIL;
    ch_gc_push(&vm->gc, root_list);

    ChReader reader;
    ch_reader_init(&reader, &vm->gc, src, len);
    int rc = CH_EXIT_OK;
    for (;;) {
        ChValue expr = CH_NIL;
        ch_gc_push(&vm->gc, &expr);
        ChReadStatus st = read_cli_datum(vm, &reader, &expr);
        if (st == CH_READ_EOF) {
            ch_gc_pop(&vm->gc);
            break;
        }
        if (st != CH_READ_OK) {
            report_reader_diag(path, src, len, &reader);
            ch_gc_pop(&vm->gc);
            rc = CH_EXIT_ERROR;
            break;
        }

        ChValue expanded = CH_NIL;
        ch_gc_push(&vm->gc, &expanded);
        char err[256];
        if (ch_expand_toplevel(vm, expr, &expanded, err, sizeof(err)) != CH_EXPAND_OK) {
            ChDiagCode code = ch_diag_classify_message(err, CH_DIAG_STAGE_COMPILE);
            ch_diag_report_simple(stderr, path, 0, 0, code, NULL, err);
            ch_gc_pop_n(&vm->gc, 2);
            rc = CH_EXIT_ERROR;
            break;
        }

        if (expanded != CH_VOID) {
            ChCompiler compiler;
            ch_compiler_init(&compiler, vm);
            ChFunction *fn = NULL;
            if (ch_compile_toplevel(&compiler, expanded, &fn) != CH_COMPILE_OK || !fn) {
                report_compiler_diag(path, &compiler);
                ch_gc_pop_n(&vm->gc, 2);
                rc = CH_EXIT_ERROR;
                break;
            }
            if (append_compiled_top_level(compiled, fn) != 0) {
                fprintf(stderr, "compile: out of memory while storing bytecode\n");
                ch_gc_pop_n(&vm->gc, 2);
                rc = CH_EXIT_ERROR;
                break;
            }
            ChValue fn_value = ch_make_pointer(&fn->header);
            ch_gc_push(&vm->gc, &fn_value);
            *root_list = ch_gc_cons(&vm->gc, fn_value, *root_list);
            ch_gc_pop(&vm->gc);
        }

        ch_gc_pop_n(&vm->gc, 2);
    }

    if (rc != CH_EXIT_OK) {
        free(compiled->fns);
        compiled->fns = NULL;
        compiled->count = 0;
        compiled->cap = 0;
    }
    return rc;
}

static void disassemble_functions(FILE *out, ChFunction **fns, size_t count) {
    for (size_t i = 0; i < count; i++) {
        fprintf(out, "; form %zu\n", i + 1);
        ch_disassemble_function(out, fns[i]);
    }
}

static int disassemble_source(ChVM *vm, const char *path) {
    ChVM probe;
    ch_vm_init(&probe);
    ch_vm_register_primitives(&probe);
    probe.script_path = path;
    for (size_t i = 0; i < vm->lib_path_count; i++) {
        probe.lib_paths[probe.lib_path_count++] = vm->lib_paths[i];
    }

    size_t len = 0;
    char *src = ch_read_file(path, &len);
    if (!src) {
        fprintf(stderr, "Error opening file '%s'\n", path);
        ch_vm_deinit(&probe);
        return CH_EXIT_ERROR;
    }

    ChCompiledTopLevels compiled;
    ChValue root_list = CH_NIL;
    int rc = compile_source_top_levels(&probe, path, src, len, &compiled, &root_list);
    if (rc == CH_EXIT_OK) {
        fprintf(stderr, "; disassembly: %s (%zu function%s)\n", path, compiled.count,
                compiled.count == 1 ? "" : "s");
        disassemble_functions(stderr, compiled.fns, compiled.count);
    }

    ch_gc_pop(&probe.gc);
    free(compiled.fns);
    free(src);
    ch_vm_deinit(&probe);
    return rc;
}

static char *default_bytecode_output_path(const char *path) {
    size_t len = strlen(path);
    if (len >= 4 && strcmp(path + len - 4, ".scm") == 0) {
        char *out = (char *)malloc(len + 2);
        if (!out) {
            return NULL;
        }
        memcpy(out, path, len - 4);
        memcpy(out + len - 4, ".chbc", 6);
        return out;
    }
    char *out = (char *)malloc(len + 6);
    if (!out) {
        return NULL;
    }
    memcpy(out, path, len);
    memcpy(out + len, ".chbc", 6);
    return out;
}

static int cmd_bytecode_compile(ChCliOptions *opts) {
    if (!opts->file) {
        fprintf(stderr, "chaaya: --compile requires a Scheme file argument\n");
        return CH_EXIT_USAGE;
    }

    size_t len = 0;
    char *src = ch_read_file(opts->file, &len);
    if (!src) {
        fprintf(stderr, "Error opening file '%s'\n", opts->file);
        return CH_EXIT_ERROR;
    }

    ChVM vm;
    ch_vm_init(&vm);
    ch_vm_register_primitives(&vm);
    apply_opts_to_vm(&vm, opts);

    ChCompiledTopLevels compiled;
    ChValue root_list = CH_NIL;
    int rc = compile_source_top_levels(&vm, opts->file, src, len, &compiled, &root_list);
    if (rc != CH_EXIT_OK) {
        ch_gc_pop(&vm.gc);
        free(src);
        ch_vm_deinit(&vm);
        return CH_EXIT_ERROR;
    }

    if (opts->flag_disassemble) {
        fprintf(stderr, "; disassembly: %s (%zu function%s)\n", opts->file, compiled.count,
                compiled.count == 1 ? "" : "s");
        disassemble_functions(stderr, compiled.fns, compiled.count);
    }

    ChFunction *empty[1] = {NULL};
    ChFunction **store_fns = compiled.count > 0 ? compiled.fns : empty;
    int cache_rc = ch_cache_store(&vm, opts->file, src, len, store_fns, compiled.count);

    char *generated_output = NULL;
    const char *out_path = opts->output;
    if (!out_path) {
        generated_output = default_bytecode_output_path(opts->file);
        if (!generated_output) {
            fprintf(stderr, "compile: out of memory while creating output path\n");
            ch_gc_pop(&vm.gc);
            free(compiled.fns);
            free(src);
            ch_vm_deinit(&vm);
            return CH_EXIT_ERROR;
        }
        out_path = generated_output;
    }

    if (ch_cache_write_file(out_path, &vm, opts->file, src, len, store_fns, compiled.count) != 0) {
        fprintf(stderr, "compile: failed to write bytecode file '%s'\n", out_path);
        rc = CH_EXIT_ERROR;
    } else {
        printf("compile: wrote %s (%zu function%s)\n", out_path, compiled.count,
               compiled.count == 1 ? "" : "s");
        if (cache_rc != 0) {
            fprintf(stderr, "compile: warning: cache directory write skipped\n");
        }
    }

    if (ch_timings_enabled() && rc == CH_EXIT_OK) {
        ch_timings_set_output(out_path);
    }

    free(generated_output);
    ch_gc_pop(&vm.gc);
    free(compiled.fns);
    free(src);
    ch_vm_deinit(&vm);
    return rc;
}

static int source_has_import(const char *src, size_t len) {
    /* Cheap skip: auto-cache disabled when the file mentions import. */
    (void)len;
    return strstr(src, "import") != NULL;
}

static int eval_function_list(ChVM *vm, const char *path, ChFunction **fns, size_t n) {
    ChValue *keeps = (ChValue *)calloc(n ? n : 1, sizeof(ChValue));
    if (!keeps) {
        return CH_EXIT_ERROR;
    }
    for (size_t i = 0; i < n; i++) {
        keeps[i] = ch_make_pointer(&fns[i]->header);
        ch_gc_push(&vm->gc, &keeps[i]);
    }
    int rc = CH_EXIT_OK;
    for (size_t i = 0; i < n; i++) {
        ChValue result = CH_VOID;
        ChVMStatus st = ch_vm_eval_function(vm, fns[i], &result);
        if (st != CH_VM_OK || (vm->error[0] && result == CH_UNDEFINED)) {
            ChDiagCode code = vm->error_code
                                  ? vm->error_code
                                  : ch_diag_classify_message(ch_vm_error(vm), CH_DIAG_STAGE_RUNTIME);
            ch_diag_report_simple(stderr, path, vm->error_line, vm->error_column, code, NULL,
                                  ch_vm_error(vm));
            rc = CH_EXIT_ERROR;
            break;
        }
    }
    ch_gc_pop_n(&vm->gc, n);
    free(keeps);
    return rc;
}

/* Compile+eval in a fresh VM so bytecode global indices match a cold cache load. */
static int cache_store_from_source(const char *path, const char *src, size_t len) {
    ChVM cold;
    ch_vm_init(&cold);
    ch_vm_register_primitives(&cold);
    cold.script_path = path;

    ChReader reader;
    ch_reader_init(&reader, &cold.gc, src, len);
    ChFunction **compiled = NULL;
    ChValue root_list = CH_NIL;
    ch_gc_push(&cold.gc, &root_list);
    size_t compiled_n = 0;
    size_t compiled_cap = 0;
    int ok = 1;

    for (;;) {
        ChValue expr = CH_NIL;
        ch_gc_push(&cold.gc, &expr);
        ChReadStatus rs = ch_read_datum(&reader, &expr);
        if (rs == CH_READ_EOF) {
            ch_gc_pop(&cold.gc);
            break;
        }
        if (rs != CH_READ_OK) {
            ch_gc_pop(&cold.gc);
            ok = 0;
            break;
        }
        if (ch_is_pair(expr) && ch_is_symbol(ch_car(expr))) {
            const char *h = ch_symbol_basename(ch_as_symbol(ch_car(expr)));
            if (strcmp(h, "import") == 0 || strcmp(h, "define-library") == 0 ||
                strcmp(h, "include") == 0 || strcmp(h, "include-ci") == 0 ||
                strcmp(h, "cond-expand") == 0) {
                ch_gc_pop(&cold.gc);
                ok = 0;
                break;
            }
        }
        ChCompiler compiler;
        ch_compiler_init(&compiler, &cold);
        ChFunction *fn = NULL;
        if (ch_compile_toplevel(&compiler, expr, &fn) != CH_COMPILE_OK || !fn) {
            ch_gc_pop(&cold.gc);
            ok = 0;
            break;
        }
        if (compiled_n >= compiled_cap) {
            size_t ncap = compiled_cap ? compiled_cap * 2 : 8;
            ChFunction **ncompiled =
                (ChFunction **)realloc(compiled, ncap * sizeof(ChFunction *));
            if (!ncompiled) {
                ch_gc_pop(&cold.gc);
                ok = 0;
                break;
            }
            compiled = ncompiled;
            compiled_cap = ncap;
        }
        compiled[compiled_n] = fn;
        ChValue fn_value = ch_make_pointer(&fn->header);
        ch_gc_push(&cold.gc, &fn_value);
        root_list = ch_gc_cons(&cold.gc, fn_value, root_list);
        ch_gc_pop(&cold.gc);
        compiled_n++;
        ch_gc_pop(&cold.gc);
        /* Compile-only: DEFINE interns global names; values are filled on cache load.
         * Skip files that need runtime define-syntax via the special-form guard above. */
    }

    int stored = -1;
    if (ok) {
        ChFunction *empty[1] = {NULL};
        stored = ch_cache_store(&cold, path, src, len, compiled_n > 0 ? compiled : empty, compiled_n);
    }
    ch_gc_pop(&cold.gc);
    free(compiled);
    ch_vm_deinit(&cold);
    return stored;
}

static int cache_disabled_by_env(void) {
    const char *v = getenv("CHAAYA_NO_CACHE");
    return v && v[0] != '\0' && strcmp(v, "0") != 0;
}

static int run_file(ChVM *vm, const ChCliOptions *opts, const char *path) {
    size_t len = 0;
    char *src = ch_read_file(path, &len);
    if (!src) {
        fprintf(stderr, "Error opening file '%s'\n", path);
        return CH_EXIT_ERROR;
    }

    int cache_off_env = cache_disabled_by_env();
    int has_import = source_has_import(src, len);
    int use_cache = !cache_off_env && !has_import && !opts->flag_no_ir_opt;
    if (ch_timings_enabled() && !use_cache) {
        const char *reason = "source has import";
        if (cache_off_env) {
            reason = "CHAAYA_NO_CACHE";
        } else if (opts->flag_no_ir_opt) {
            reason = "--no-ir-opt";
        }
        ch_timings_set_cache(CH_TIMINGS_CACHE_OFF, path, 0, reason);
    }

    if (use_cache) {
        ChFunction **fns = NULL;
        size_t n = 0;
        if (ch_cache_try_load(vm, path, src, len, &fns, &n)) {
            if (opts->flag_disassemble) {
                ChValue rooted_fns = CH_NIL;
                ch_gc_push(&vm->gc, &rooted_fns);
                for (size_t i = 0; i < n; i++) {
                    ChValue fn_value = ch_make_pointer(&fns[i]->header);
                    ch_gc_push(&vm->gc, &fn_value);
                    rooted_fns = ch_gc_cons(&vm->gc, fn_value, rooted_fns);
                    ch_gc_pop(&vm->gc);
                }
                fprintf(stderr, "; disassembly: cache-hit %s (%zu function%s)\n", path, n,
                        n == 1 ? "" : "s");
                disassemble_functions(stderr, fns, n);
                ch_gc_pop(&vm->gc);
            }
            if (ch_timings_enabled()) {
                ch_timings_set_cache(CH_TIMINGS_CACHE_HIT, path, 0, NULL);
                ch_timings_begin(CH_TIMINGS_EXECUTE);
            }
            int rc = eval_function_list(vm, path, fns, n);
            if (ch_timings_enabled()) {
                ch_timings_end(CH_TIMINGS_EXECUTE);
            }
            free(fns);
            free(src);
            return rc;
        }
    }

    if (opts->flag_disassemble) {
        if (disassemble_source(vm, path) != CH_EXIT_OK) {
            free(src);
            return CH_EXIT_ERROR;
        }
    }

    if (ch_timings_enabled()) {
        ch_timings_begin(CH_TIMINGS_EXECUTE);
    }
    int rc = ch_eval_source(vm, src, len, 0);
    if (ch_timings_enabled()) {
        ch_timings_end(CH_TIMINGS_EXECUTE);
    }

    if (rc == 0 && use_cache) {
        int stored = cache_store_from_source(path, src, len);
        if (ch_timings_enabled()) {
            ch_timings_set_cache(CH_TIMINGS_CACHE_MISS, path, stored == 0,
                                 stored == 0 ? NULL : "store failed");
        }
    } else if (ch_timings_enabled() && use_cache) {
        ch_timings_set_cache(CH_TIMINGS_CACHE_MISS, path, 0, "evaluation failed");
    }
    free(src);
    return rc == 0 ? CH_EXIT_OK : CH_EXIT_ERROR;
}

static int run_stdin(ChVM *vm) {
    size_t cap = 4096;
    size_t len = 0;
    char *buf = (char *)malloc(cap);
    if (!buf) {
        return CH_EXIT_ERROR;
    }
    for (;;) {
        if (len + 1024 >= cap) {
            cap *= 2;
            char *n = (char *)realloc(buf, cap);
            if (!n) {
                free(buf);
                return CH_EXIT_ERROR;
            }
            buf = n;
        }
        size_t n = fread(buf + len, 1, 1024, stdin);
        len += n;
        if (n < 1024) {
            break;
        }
    }
    buf[len] = '\0';
    int rc = ch_eval_source(vm, buf, len, 0);
    free(buf);
    return rc;
}

static void apply_opts_to_vm(ChVM *vm, ChCliOptions *opts) {
    for (size_t i = 0; i < opts->lib_path_count; i++) {
        vm->lib_paths[vm->lib_path_count++] = opts->lib_paths[i];
    }
    vm->script_path = opts->file;
    for (size_t i = 0; i < opts->script_arg_count; i++) {
        vm->script_args[vm->script_arg_count++] = opts->script_args[i];
    }
}

static int apply_process_memory_limit(size_t bytes) {
    if (bytes == 0) {
        return 0;
    }
#if defined(RLIMIT_AS)
    {
        struct rlimit lim = {.rlim_cur = (rlim_t)bytes, .rlim_max = (rlim_t)bytes};
        if (setrlimit(RLIMIT_AS, &lim) == 0) {
            return 0;
        }
    }
#endif
#if defined(RLIMIT_DATA)
    {
        struct rlimit lim = {.rlim_cur = (rlim_t)bytes, .rlim_max = (rlim_t)bytes};
        if (setrlimit(RLIMIT_DATA, &lim) == 0) {
            return 0;
        }
    }
#endif
    return -1;
}

static void print_gc_stats(FILE *out, const ChVM *vm) {
    fprintf(out, "GC Statistics:\n");
    fprintf(out, "  objects=%zu young=%zu old=%zu\n", vm->gc.object_count, vm->gc.young_count,
            vm->gc.old_count);
    fprintf(out, "  alloc_count=%zu threshold=%zu\n", vm->gc.alloc_count, vm->gc.threshold);
    fprintf(out, "  collections=%zu minor=%zu major=%zu\n", vm->gc.collections,
            vm->gc.minor_collections, vm->gc.major_collections);
}

int ch_cli_dispatch(ChCliOptions *opts, int argc, char **argv) {
    (void)argc;
    const char *argv0 = argv[0];

    if (opts->flag_diagnostics) {
        ch_diag_set_format(opts->diagnostics_format);
    }

    if (opts->flag_timings) {
        ch_timings_enable(opts->timings_json ? CH_TIMINGS_JSON : CH_TIMINGS_TEXT);
    }
    if (opts->flag_sandbox) {
        ch_sandbox_enable();
    }
    if (opts->flag_profile || opts->flag_profile_json) {
        ch_profile_enable();
        ch_profile_enter("run");
    }
    if (opts->flag_coverage || opts->flag_coverage_xml) {
        ch_coverage_enable();
    }

    if (opts->help) {
        ch_cli_print_help(argv0);
        return CH_EXIT_OK;
    }
    if (opts->version) {
        ch_cli_print_version();
        return CH_EXIT_OK;
    }
    if (opts->completions_shell) {
        if (strcmp(opts->completions_shell, "bash") != 0 &&
            strcmp(opts->completions_shell, "zsh") != 0 &&
            strcmp(opts->completions_shell, "fish") != 0) {
            print_completions(opts->completions_shell);
            return CH_EXIT_USAGE;
        }
        print_completions(opts->completions_shell);
        return CH_EXIT_OK;
    }

    if (opts->flag_emit_llvm) {
        if (!opts->file) {
            fprintf(stderr, "chaaya: --emit-llvm requires a Scheme file argument\n");
            usage_hint(argv0);
            return CH_EXIT_USAGE;
        }
        return ch_llvm_backend_emit_ir(opts->file, opts->output);
    }

    if (opts->flag_compile) {
        if (ch_timings_enabled()) {
            ch_timings_set_mode(CH_TIMINGS_MODE_COMPILE);
        }
        return cmd_bytecode_compile(opts);
    }

    if (opts->flag_native || opts->command == CH_CMD_COMPILE) {
        if (!opts->file) {
            fprintf(stderr, "chaaya: compile/--native requires a Scheme file argument\n");
            usage_hint(argv0);
            return CH_EXIT_USAGE;
        }
        if (opts->flag_native && opts->command != CH_CMD_COMPILE) {
            /* --native: compile to a temp binary, run it, delete it. */
            return ch_llvm_backend_run_file(opts->file);
        }
        return ch_llvm_backend_compile_native(opts->file, opts->output);
    }

    /* NYI flags on an otherwise valid command */
    if (opts->nyi_flag) {
        return not_implemented(opts->nyi_flag);
    }

    switch (opts->command) {
    case CH_CMD_FEATURES:
        return cmd_features(opts);
    case CH_CMD_DOCTOR:
        return ch_doctor_run(opts);
    case CH_CMD_LSP:
        return ch_lsp_run_stdio();
    case CH_CMD_WASM:
        return cmd_wasm_stub();
    case CH_CMD_AST:
        if (!opts->file) {
            fprintf(stderr, "ast: missing file\n");
            usage_hint(argv0);
            return CH_EXIT_USAGE;
        }
        return cmd_ast(opts->file);
    case CH_CMD_CACHE_STATUS:
        return ch_cache_status();
    case CH_CMD_CACHE_CLEAR:
        return ch_cache_clear();
    case CH_CMD_COMPILE:
        /* handled above with --native */
        return CH_EXIT_ERROR;
    case CH_CMD_CHECK:
        if (!opts->file) {
            fprintf(stderr, "check: missing file\n");
            usage_hint(argv0);
            return CH_EXIT_USAGE;
        }
        return cmd_check(opts->file, opts);
    case CH_CMD_EXPLAIN:
        if (opts->explain_all) {
            return ch_diag_explain_all(opts->json);
        }
        if (!opts->explain_code) {
            fprintf(stderr, "explain: missing diagnostic code\n");
            usage_hint(argv0);
            return CH_EXIT_USAGE;
        }
        return ch_diag_explain(opts->explain_code, opts->json);
    case CH_CMD_TEST:
        return cmd_test(argv0, opts);
    case CH_CMD_EXPAND:
        if (!opts->file) {
            fprintf(stderr, "expand: missing file\n");
            usage_hint(argv0);
            return CH_EXIT_USAGE;
        }
        return cmd_expand(opts->file, opts);
    case CH_CMD_IR:
        if (!opts->file) {
            fprintf(stderr, "ir: missing file\n");
            usage_hint(argv0);
            return CH_EXIT_USAGE;
        }
        return cmd_ir(opts->file, opts);
    case CH_CMD_FMT:
        if (!opts->file) {
            fprintf(stderr, "fmt: missing file\n");
            usage_hint(argv0);
            return CH_EXIT_USAGE;
        }
        return cmd_fmt(opts->file, opts);
    case CH_CMD_RUN:
        break;
    }

    ChVM vm;
    ch_vm_init(&vm);
    ch_vm_register_primitives(&vm);
    apply_opts_to_vm(&vm, opts);

    if (opts->flag_max_memory && apply_process_memory_limit(opts->max_memory_bytes) != 0) {
        fprintf(stderr, "warning: failed to apply --max-memory=%zu\n", opts->max_memory_bytes);
    }

    struct sigaction old_alarm_action;
    int have_old_alarm_action = 0;
    g_cli_timeout_fired = 0;
    if (opts->flag_timeout && opts->file && opts->timeout_ms > 0) {
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = cli_timeout_handler;
        sigemptyset(&sa.sa_mask);
        if (sigaction(SIGALRM, &sa, &old_alarm_action) == 0) {
            have_old_alarm_action = 1;
            unsigned long ms = (unsigned long)opts->timeout_ms;
            unsigned seconds = (unsigned)((ms + 999ul) / 1000ul);
            if (seconds == 0) {
                seconds = 1;
            }
            alarm(seconds);
        } else {
            fprintf(stderr, "warning: failed to install timeout handler\n");
        }
    }

    int rc;
    const char *emit_path = ch_test_worker_emit_path();
    if (emit_path && opts->file) {
        /* Auto-add bundled lib/ when worker has no --lib-path (SRFI-64). */
        if (opts->lib_path_count == 0) {
            static const char *bundled_lib = CHAAYA_SOURCE_DIR "/lib";
            opts->lib_paths[0] = bundled_lib;
            opts->lib_path_count = 1;
            apply_opts_to_vm(&vm, opts);
        }
        rc = ch_test_run_worker_file(&vm, opts->file, emit_path);
    } else if (opts->file) {
        rc = run_file(&vm, opts, opts->file);
    } else if (isatty(STDIN_FILENO)) {
        rc = ch_repl_run(&vm);
    } else {
        rc = run_stdin(&vm);
    }

    if (opts->flag_timeout && opts->file && have_old_alarm_action) {
        alarm(0);
        sigaction(SIGALRM, &old_alarm_action, NULL);
    }
    if (g_cli_timeout_fired) {
        ch_diag_report_simple(stderr, opts->file, 0, 0, CH_DIAG_EXECUTION_TIMEOUT, NULL,
                              "execution timed out");
        rc = CH_EXIT_ERROR;
    }

    if (opts->flag_gc_stats) {
        print_gc_stats(stderr, &vm);
    }
    if (opts->flag_timings) {
        ch_timings_report(stderr);
    }
    if (opts->flag_profile || opts->flag_profile_json) {
        ch_profile_leave("run");
        if (opts->flag_profile) {
            ch_profile_report_text(stderr);
        }
        if (opts->flag_profile_json && opts->profile_json_path &&
            ch_profile_report_json(opts->profile_json_path) != 0) {
            fprintf(stderr, "profile: failed to write %s\n", opts->profile_json_path);
            rc = CH_EXIT_ERROR;
        }
    }
    if (opts->flag_coverage || opts->flag_coverage_xml) {
        if (opts->flag_coverage) {
            ch_coverage_report_text(stderr);
        }
        if (opts->flag_coverage_xml && opts->coverage_xml_path &&
            ch_coverage_report_xml(opts->coverage_xml_path) != 0) {
            fprintf(stderr, "coverage: failed to write %s\n", opts->coverage_xml_path);
            rc = CH_EXIT_ERROR;
        }
    }

    ch_vm_deinit(&vm);
    return rc;
}
