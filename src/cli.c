#include "chaaya/cli.h"

#include "chaaya/cache.h"
#include "chaaya/compiler.h"
#include "chaaya/eval.h"
#include "chaaya/expander.h"
#include "chaaya/features.h"
#include "chaaya/library.h"
#include "chaaya/llvm_backend.h"
#include "chaaya/lsp.h"
#include "chaaya/opcode.h"
#include "chaaya/printer.h"
#include "chaaya/reader.h"
#include "chaaya/repl.h"
#include "chaaya/version.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <libgen.h>

static void apply_opts_to_vm(ChVM *vm, ChCliOptions *opts);

static void usage_hint(const char *argv0) {
    fprintf(stderr, "Run '%s --help' for usage.\n", argv0);
}

void ch_cli_print_version(void) {
    printf("%s\n", CHAAYA_VERSION_BANNER);
}

void ch_cli_print_help(const char *argv0) {
    printf("%s\n\n", CHAAYA_VERSION_BANNER);
    printf("Usage: %s [options] [file] [script-args...]\n", argv0);
    printf("       %s compile <file.scm> [-o output]\n", argv0);
    printf("       %s check <file.scm>\n", argv0);
    printf("       %s explain <code>\n", argv0);
    printf("       %s features [--json]\n", argv0);
    printf("       %s test [paths...]\n", argv0);
    printf("       %s ast|expand|ir <file.scm>\n", argv0);
    printf("       %s doctor [--json]\n", argv0);
    printf("       %s lsp\n", argv0);
    printf("       %s wasm\n", argv0);
    printf("       %s fmt [--check] [files...]\n", argv0);
    printf("       %s cache <status|clear>\n", argv0);
    printf("\n");
    printf("Commands:\n");
    printf("  compile <file>     Compile to native binary via LLVM\n");
    printf("  check <file>       Compile-only static analysis (no execution)\n");
    printf("  explain <code>     Explain a diagnostic code (e.g. KP3001)\n");
    printf("  features           Report this build's capabilities; --json\n");
    printf("  test [paths...]    Run SRFI-64 suites\n");
    printf("  ast <file>         Print post-read datums (read + write)\n");
    printf("  expand <file>      Print the program after full macro expansion\n");
    printf("  ir <file> [--no-opt]  Print the IR tree\n");
    printf("  doctor [--json]    Check the installation and environment\n");
    printf("  lsp                Run a minimal stdio JSON-RPC loop\n");
    printf("  wasm               WebAssembly backend stub (MVP)\n");
    printf("  fmt [files...]     Canonically format Scheme in place; --check\n");
    printf("  cache status       Show the bytecode cache location and entries\n");
    printf("  cache clear        Remove all bytecode cache entries\n");
    printf("\n");
    printf("Options:\n");
    printf("  -h, --help         Show this help message\n");
    printf("  --version          Show version\n");
    printf("  -v                 Show version (alias)\n");
    printf("  --lib-path <path>  Add library search path (repeatable)\n");
    printf("  --native           Route file execution through LLVM backend stub\n");
    printf("  --compile          Compile file to bytecode (.sbc)\n");
    printf("  --emit-llvm        Emit LLVM IR text (.ll)\n");
    printf("  -o <file>          Output path\n");
    printf("  --disassemble      Disassemble bytecode\n");
    printf("  --diagnostics=<fmt> Diagnostic output format: text or json\n");
    printf("  --deny-warnings    (check) Treat lint warnings as errors\n");
    printf("  --no-ir-opt        Disable IR optimization passes\n");
    printf("  --sandbox          Restrict filesystem and process access\n");
    printf("  --gc-stats         Print GC statistics on exit\n");
    printf("  --profile          Enable profiling\n");
    printf("  --profile-json <f> Write profile JSON to file\n");
    printf("  --timings[=fmt]    Report per-stage pipeline timings\n");
    printf("  --coverage         Report library procedure coverage\n");
    printf("  --coverage-xml <f> Write Cobertura XML coverage to file\n");
    printf("  --timeout <ms>     Execution timeout in milliseconds\n");
    printf("  --max-memory <n>   Maximum heap memory in bytes\n");
    printf("  --completions <sh> Output completion script (bash, zsh, fish)\n");
    printf("\n");
    printf("Environment variables:\n");
    printf("  CHAAYA_HOME        Override ~/.chaaya (history, future cache/lib)\n");
    printf("  CHAAYA_LIB_DIR     Directory for native runtime libs (future)\n");
    printf("\n");
    printf("With no file argument, starts an interactive REPL.\n");
    printf("With no file and non-TTY stdin, evaluates stdin as a script.\n");
}

static int mark_nyi(ChCliOptions *o, const char *flag) {
    if (!o->nyi_flag) {
        o->nyi_flag = flag;
    }
    return 0;
}

static int is_subcommand(const char *s) {
    return strcmp(s, "compile") == 0 || strcmp(s, "check") == 0 || strcmp(s, "explain") == 0 ||
           strcmp(s, "features") == 0 || strcmp(s, "test") == 0 || strcmp(s, "ast") == 0 ||
           strcmp(s, "expand") == 0 || strcmp(s, "ir") == 0 || strcmp(s, "doctor") == 0 ||
           strcmp(s, "lsp") == 0 || strcmp(s, "wasm") == 0 || strcmp(s, "fmt") == 0 ||
           strcmp(s, "cache") == 0;
}

int ch_cli_parse(int argc, char **argv, ChCliOptions *out) {
    memset(out, 0, sizeof(*out));
    out->command = CH_CMD_RUN;
    const char *argv0 = argc > 0 ? argv[0] : "chaaya";

    int i = 1;
    /* Optional leading subcommand */
    if (i < argc && argv[i][0] != '-' && is_subcommand(argv[i])) {
        const char *cmd = argv[i++];
        if (strcmp(cmd, "compile") == 0) {
            out->command = CH_CMD_COMPILE;
        } else if (strcmp(cmd, "check") == 0) {
            out->command = CH_CMD_CHECK;
        } else if (strcmp(cmd, "explain") == 0) {
            out->command = CH_CMD_EXPLAIN;
        } else if (strcmp(cmd, "features") == 0) {
            out->command = CH_CMD_FEATURES;
        } else if (strcmp(cmd, "test") == 0) {
            out->command = CH_CMD_TEST;
        } else if (strcmp(cmd, "ast") == 0) {
            out->command = CH_CMD_AST;
        } else if (strcmp(cmd, "expand") == 0) {
            out->command = CH_CMD_EXPAND;
        } else if (strcmp(cmd, "ir") == 0) {
            out->command = CH_CMD_IR;
        } else if (strcmp(cmd, "doctor") == 0) {
            out->command = CH_CMD_DOCTOR;
        } else if (strcmp(cmd, "lsp") == 0) {
            out->command = CH_CMD_LSP;
        } else if (strcmp(cmd, "wasm") == 0) {
            out->command = CH_CMD_WASM;
        } else if (strcmp(cmd, "fmt") == 0) {
            out->command = CH_CMD_FMT;
        } else if (strcmp(cmd, "cache") == 0) {
            if (i >= argc) {
                fprintf(stderr, "cache: missing subcommand (status|clear)\n");
                usage_hint(argv0);
                return CH_EXIT_USAGE;
            }
            if (strcmp(argv[i], "status") == 0) {
                out->command = CH_CMD_CACHE_STATUS;
            } else if (strcmp(argv[i], "clear") == 0) {
                out->command = CH_CMD_CACHE_CLEAR;
            } else {
                fprintf(stderr, "cache: unknown subcommand '%s'\n", argv[i]);
                usage_hint(argv0);
                return CH_EXIT_USAGE;
            }
            i++;
        }
    }

    for (; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
            out->help = 1;
            continue;
        }
        if (strcmp(a, "--version") == 0 || strcmp(a, "-v") == 0) {
            out->version = 1;
            continue;
        }
        if (strcmp(a, "--json") == 0) {
            out->json = 1;
            continue;
        }
        if (strcmp(a, "--lib-path") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "--lib-path requires a path\n");
                usage_hint(argv0);
                return CH_EXIT_USAGE;
            }
            if (out->lib_path_count >= CH_VM_MAX_LIB_PATHS) {
                fprintf(stderr, "too many --lib-path entries\n");
                return CH_EXIT_USAGE;
            }
            out->lib_paths[out->lib_path_count++] = argv[++i];
            continue;
        }
        if (strcmp(a, "--native") == 0) {
            out->flag_native = 1;
            continue;
        }
        if (strcmp(a, "--completions") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "--completions requires bash, zsh, or fish\n");
                usage_hint(argv0);
                return CH_EXIT_USAGE;
            }
            out->completions_shell = argv[++i];
            continue;
        }
        if (strcmp(a, "-o") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "-o requires a path\n");
                usage_hint(argv0);
                return CH_EXIT_USAGE;
            }
            out->output = argv[++i];
            continue;
        }
        if (strcmp(a, "--compile") == 0) {
            out->flag_compile = 1;
            mark_nyi(out, "--compile");
            continue;
        }
        if (strcmp(a, "--emit-llvm") == 0) {
            out->flag_emit_llvm = 1;
            mark_nyi(out, "--emit-llvm");
            continue;
        }
        if (strcmp(a, "--disassemble") == 0) {
            out->flag_disassemble = 1;
            mark_nyi(out, "--disassemble");
            continue;
        }
        if (strcmp(a, "--sandbox") == 0) {
            out->flag_sandbox = 1;
            mark_nyi(out, "--sandbox");
            continue;
        }
        if (strcmp(a, "--gc-stats") == 0) {
            out->flag_gc_stats = 1;
            mark_nyi(out, "--gc-stats");
            continue;
        }
        if (strcmp(a, "--profile") == 0) {
            out->flag_profile = 1;
            mark_nyi(out, "--profile");
            continue;
        }
        if (strcmp(a, "--coverage") == 0) {
            out->flag_coverage = 1;
            mark_nyi(out, "--coverage");
            continue;
        }
        if (strcmp(a, "--no-ir-opt") == 0 || strcmp(a, "--no-opt") == 0) {
            out->flag_no_ir_opt = 1;
            mark_nyi(out, a);
            continue;
        }
        if (strcmp(a, "--deny-warnings") == 0) {
            out->flag_deny_warnings = 1;
            mark_nyi(out, "--deny-warnings");
            continue;
        }
        if (strcmp(a, "--timings") == 0 || strncmp(a, "--timings=", 10) == 0) {
            out->flag_timings = 1;
            mark_nyi(out, "--timings");
            continue;
        }
        if (strncmp(a, "--diagnostics=", 14) == 0) {
            out->flag_diagnostics = 1;
            mark_nyi(out, "--diagnostics");
            continue;
        }
        if (strcmp(a, "--timeout") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "--timeout requires a value\n");
                return CH_EXIT_USAGE;
            }
            i++;
            out->flag_timeout = 1;
            mark_nyi(out, "--timeout");
            continue;
        }
        if (strcmp(a, "--max-memory") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "--max-memory requires a value\n");
                return CH_EXIT_USAGE;
            }
            i++;
            out->flag_max_memory = 1;
            mark_nyi(out, "--max-memory");
            continue;
        }
        if (strcmp(a, "--profile-json") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "--profile-json requires a path\n");
                return CH_EXIT_USAGE;
            }
            i++;
            out->flag_profile_json = 1;
            mark_nyi(out, "--profile-json");
            continue;
        }
        if (strcmp(a, "--coverage-xml") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "--coverage-xml requires a path\n");
                return CH_EXIT_USAGE;
            }
            i++;
            out->flag_coverage_xml = 1;
            mark_nyi(out, "--coverage-xml");
            continue;
        }
        if (strcmp(a, "--check") == 0) {
            if (out->command == CH_CMD_FMT) {
                out->flag_fmt_check = 1;
            } else {
                mark_nyi(out, "--check");
            }
            continue;
        }
        if (a[0] == '-') {
            fprintf(stderr, "unknown option: %s\n", a);
            usage_hint(argv0);
            return CH_EXIT_USAGE;
        }

        /* positional */
        if (out->command == CH_CMD_EXPLAIN && !out->explain_code) {
            out->explain_code = a;
            continue;
        }
        if (!out->file && (out->command == CH_CMD_RUN || out->command == CH_CMD_AST ||
                           out->command == CH_CMD_EXPAND || out->command == CH_CMD_IR ||
                           out->command == CH_CMD_COMPILE || out->command == CH_CMD_CHECK ||
                           out->command == CH_CMD_FMT || out->command == CH_CMD_TEST)) {
            out->file = a;
            continue;
        }
        if (out->command == CH_CMD_TEST) {
            if (out->script_arg_count >= CH_VM_MAX_SCRIPT_ARGS) {
                fprintf(stderr, "too many test paths\n");
                return CH_EXIT_USAGE;
            }
            out->script_args[out->script_arg_count++] = a;
            continue;
        }
        if (out->command == CH_CMD_RUN && out->file) {
            if (out->script_arg_count >= CH_VM_MAX_SCRIPT_ARGS) {
                fprintf(stderr, "too many script arguments\n");
                return CH_EXIT_USAGE;
            }
            out->script_args[out->script_arg_count++] = a;
            continue;
        }
        fprintf(stderr, "unexpected argument: %s\n", a);
        usage_hint(argv0);
        return CH_EXIT_USAGE;
    }
    return CH_EXIT_OK;
}

static int not_implemented(const char *what) {
    fprintf(stderr, "chaaya: '%s' is not implemented yet (bootstrap)\n", what);
    return CH_EXIT_ERROR;
}

static int run_test_subprocess(const char *argv0, ChCliOptions *opts, const char *path) {
    char exe[1024];
    if (realpath(argv0, exe) == NULL) {
        if (snprintf(exe, sizeof(exe), "%s", argv0) >= (int)sizeof(exe)) {
            fprintf(stderr, "test: argv0 path too long\n");
            return -1;
        }
    }

    char dir_copy[1024];
    char file_copy[1024];
    if (snprintf(dir_copy, sizeof(dir_copy), "%s", path) >= (int)sizeof(dir_copy) ||
        snprintf(file_copy, sizeof(file_copy), "%s", path) >= (int)sizeof(file_copy)) {
        fprintf(stderr, "test: path too long: %s\n", path);
        return -1;
    }
    char *dir = dirname(dir_copy);
    const char *file = basename(file_copy);

    char lib_abs[CH_VM_MAX_LIB_PATHS][1024];
    const char *resolved_libs[CH_VM_MAX_LIB_PATHS];
    for (size_t i = 0; i < opts->lib_path_count; i++) {
        if (realpath(opts->lib_paths[i], lib_abs[i]) != NULL) {
            resolved_libs[i] = lib_abs[i];
        } else {
            resolved_libs[i] = opts->lib_paths[i];
        }
    }

    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "test: fork failed for %s\n", path);
        return -1;
    }
    if (pid == 0) {
        if (chdir(dir) != 0) {
            _exit(127);
        }
        char **child_argv = NULL;
        size_t argc = 0;
        size_t cap = 16;
        child_argv = (char **)calloc(cap, sizeof(char *));
        if (!child_argv) {
            _exit(127);
        }
        child_argv[argc++] = exe;
        for (size_t i = 0; i < opts->lib_path_count; i++) {
            if (argc + 2 >= cap) {
                cap *= 2;
                char **n = (char **)realloc(child_argv, cap * sizeof(char *));
                if (!n) {
                    free(child_argv);
                    _exit(127);
                }
                child_argv = n;
            }
            child_argv[argc++] = (char *)"--lib-path";
            child_argv[argc++] = (char *)resolved_libs[i];
        }
        if (argc + 2 >= cap) {
            cap += 2;
            char **n = (char **)realloc(child_argv, cap * sizeof(char *));
            if (!n) {
                free(child_argv);
                _exit(127);
            }
            child_argv = n;
        }
        child_argv[argc++] = (char *)file;
        child_argv[argc] = NULL;
        execv(exe, child_argv);
        free(child_argv);
        _exit(127);
    }

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        fprintf(stderr, "test: waitpid failed for %s\n", path);
        return -1;
    }
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status) == 0 ? 0 : -1;
    }
    fprintf(stderr, "test: abnormal exit for %s\n", path);
    return -1;
}

static int cmd_test(const char *argv0, ChCliOptions *opts) {
    if (!opts->file) {
        fprintf(stderr, "test: missing test file (pass one or more .scm paths)\n");
        fprintf(stderr, "Usage: %s test [--lib-path ./lib] file.scm [more...]\n", argv0);
        return CH_EXIT_USAGE;
    }

    size_t path_count = 1 + opts->script_arg_count;
    const char *paths[1 + CH_VM_MAX_SCRIPT_ARGS];
    paths[0] = opts->file;
    for (size_t i = 0; i < opts->script_arg_count; i++) {
        paths[i + 1] = opts->script_args[i];
    }

    int failed = 0;
    for (size_t i = 0; i < path_count; i++) {
        const char *path = paths[i];
        if (run_test_subprocess(argv0, opts, path) == 0) {
            printf("PASS %s\n", path);
        } else {
            fprintf(stderr, "FAIL %s\n", path);
            failed++;
        }
    }

    if (failed > 0) {
        fprintf(stderr, "test: %d/%zu file(s) failed\n", failed, path_count);
        return CH_EXIT_ERROR;
    }
    printf("test: %zu file(s) passed\n", path_count);
    return CH_EXIT_OK;
}

static int cmd_wasm_stub(void) {
    fprintf(stderr, "chaaya: 'wasm' backend is not implemented yet (MVP stub).\n");
    fprintf(stderr, "Enable -DCHAAYA_WASM=ON to build the wasm target scaffold.\n");
    return CH_EXIT_ERROR;
}

static void print_completions(const char *shell) {
    if (strcmp(shell, "bash") == 0) {
        puts("# chaaya bash completion");
        puts("_chaaya() {");
        puts("  local cur=\"${COMP_WORDS[COMP_CWORD]}\"");
        puts("  local cmds=\"compile check explain features test ast expand ir doctor lsp wasm fmt "
             "cache\"");
        puts("  local opts=\"--help --version --lib-path --native --completions --compile "
             "--emit-llvm -o --disassemble --sandbox --gc-stats --profile --timings --coverage "
             "--json\"");
        puts("  COMPREPLY=( $(compgen -W \"$cmds $opts\" -- \"$cur\") )");
        puts("}");
        puts("complete -F _chaaya chaaya");
        return;
    }
    if (strcmp(shell, "zsh") == 0) {
        puts("#compdef chaaya");
        puts("_arguments \\");
        puts("  '(-h --help)'{-h,--help}'[show help]' \\");
        puts("  '--version[show version]' \\");
        puts("  '--lib-path[library path]:path:_files' \\");
        puts("  '--native[run via LLVM backend stub]' \\");
        puts("  '--completions[shell completions]:shell:(bash zsh fish)' \\");
        puts("  '1:command:(compile check explain features test ast expand ir doctor lsp wasm fmt "
             "cache)'");
        return;
    }
    if (strcmp(shell, "fish") == 0) {
        puts("complete -c chaaya -s h -l help -d 'Show help'");
        puts("complete -c chaaya -l version -d 'Show version'");
        puts("complete -c chaaya -l lib-path -r -d 'Library search path'");
        puts("complete -c chaaya -l native -d 'Run via LLVM backend stub'");
        puts("complete -c chaaya -l completions -xa 'bash zsh fish'");
        puts("complete -c chaaya -a 'compile check explain features test ast expand ir doctor lsp "
             "wasm fmt cache'");
        return;
    }
    fprintf(stderr, "unknown shell for --completions: %s (want bash, zsh, or fish)\n", shell);
}

static int cmd_features(ChCliOptions *opts) {
    if (opts->json) {
        return ch_features_print_json(stdout, opts->lib_paths, opts->lib_path_count);
    }
    ch_features_print_text(stdout);
    return CH_EXIT_OK;
}

static int path_writable_dir(const char *dir) {
    struct stat st;
    if (stat(dir, &st) != 0) {
        return 0;
    }
    return S_ISDIR(st.st_mode) && access(dir, W_OK) == 0;
}

static int cmd_doctor(ChCliOptions *opts) {
    int fails = 0;
    int warns = 0;
    char home[512];
    const char *env = getenv("CHAAYA_HOME");
    if (env && env[0]) {
        snprintf(home, sizeof(home), "%s", env);
    } else {
        const char *h = getenv("HOME");
        if (!h) {
            h = "";
        }
        snprintf(home, sizeof(home), "%s/.chaaya", h);
    }

    if (opts->json) {
        printf("{\n  \"checks\": [\n");
    }

#define DOCTOR_EMIT(status, name, detail)                                                          \
    do {                                                                                           \
        if (opts->json) {                                                                          \
            printf("    {\"status\": \"%s\", \"name\": \"%s\", \"detail\": \"%s\"},\n", status,   \
                   name, detail);                                                                  \
        } else {                                                                                   \
            printf("%s  %s — %s\n", status, name, detail);                                         \
        }                                                                                          \
    } while (0)

    DOCTOR_EMIT("PASS", "binary", "chaaya executable is running");

#ifdef CHAAYA_HAS_LINENOISE
    DOCTOR_EMIT("PASS", "linenoise", "built with linenoise");
#else
    DOCTOR_EMIT("WARN", "linenoise", "built without linenoise (plain REPL)");
    warns++;
#endif

    if (isatty(STDIN_FILENO)) {
        DOCTOR_EMIT("PASS", "tty", "stdin is a TTY");
    } else {
        DOCTOR_EMIT("WARN", "tty", "stdin is not a TTY");
        warns++;
    }

    if (path_writable_dir(home) || (getenv("HOME") && getenv("HOME")[0])) {
        /* home may not exist yet — warn only if CHAAYA_HOME set and missing */
        if (env && env[0] && !path_writable_dir(home)) {
            DOCTOR_EMIT("WARN", "chaaya_home", "CHAAYA_HOME is not a writable directory");
            warns++;
        } else {
            DOCTOR_EMIT("PASS", "chaaya_home", home);
        }
    } else {
        DOCTOR_EMIT("FAIL", "home", "HOME is unset");
        fails++;
    }

    for (size_t i = 0; i < opts->lib_path_count; i++) {
        char buf[640];
        snprintf(buf, sizeof(buf), "%s", opts->lib_paths[i]);
        if (path_writable_dir(opts->lib_paths[i]) || access(opts->lib_paths[i], R_OK) == 0) {
            DOCTOR_EMIT("PASS", "lib-path", buf);
        } else {
            DOCTOR_EMIT("WARN", "lib-path", buf);
            warns++;
        }
    }

    if (opts->json) {
        printf("    {\"status\": \"INFO\", \"name\": \"summary\", \"detail\": \"fails=%d "
               "warns=%d\"}\n",
               fails, warns);
        printf("  ]\n}\n");
    } else {
        printf("\n%d fail(s), %d warning(s)\n", fails, warns);
    }
#undef DOCTOR_EMIT
    return fails ? CH_EXIT_ERROR : CH_EXIT_OK;
}

static size_t push_cli_vm_roots(ChVM *vm) {
    for (size_t i = 0; i < vm->global_count; i++) {
        ch_gc_push(&vm->gc, &vm->globals[i].value);
    }
    for (size_t i = 0; i < vm->macro_count; i++) {
        ch_gc_push(&vm->gc, &vm->macros[i].transformer);
    }
    size_t lib_roots = ch_library_push_gc_roots(vm);
    return vm->global_count + vm->macro_count + lib_roots;
}

static ChReadStatus read_cli_datum(ChVM *vm, ChReader *reader, ChValue *out) {
    size_t sticky_roots = push_cli_vm_roots(vm);
    ChReadStatus st = ch_read_datum(reader, out);
    ch_gc_pop_n(&vm->gc, sticky_roots);
    return st;
}

static int cmd_ast(const char *path) {
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

static int cmd_expand(const char *path, const ChCliOptions *opts) {
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
        size_t sticky_roots = push_cli_vm_roots(&vm);
        char err[256];
        if (ch_expand_toplevel(&vm, v, &out, err, sizeof(err)) != CH_EXPAND_OK) {
            ch_gc_pop_n(&vm.gc, sticky_roots);
            fprintf(stderr, "expand error: %s\n", err);
            ch_gc_pop_n(&vm.gc, 2);
            free(src);
            ch_vm_deinit(&vm);
            return CH_EXIT_ERROR;
        }
        ch_gc_pop_n(&vm.gc, sticky_roots);
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

static int write_fmt_output(const char *path, const char *buf, size_t len) {
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "fmt: cannot write '%s'\n", path);
        return CH_EXIT_ERROR;
    }
    if (len > 0 && fwrite(buf, 1, len, f) != len) {
        fclose(f);
        fprintf(stderr, "fmt: write error on '%s'\n", path);
        return CH_EXIT_ERROR;
    }
    fclose(f);
    return CH_EXIT_OK;
}

static int cmd_fmt(const char *path, const ChCliOptions *opts) {
    size_t src_len = 0;
    char *src = ch_read_file(path, &src_len);
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

    char *out_buf = NULL;
    size_t out_len = 0;
    FILE *mem = open_memstream(&out_buf, &out_len);
    if (!mem) {
        free(src);
        ch_vm_deinit(&vm);
        return CH_EXIT_ERROR;
    }

    ChReader reader;
    ch_reader_init(&reader, &vm.gc, src, src_len);
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
            fprintf(stderr, "read error: %s\n", ch_reader_error(&reader));
            ch_gc_pop(&vm.gc);
            rc = CH_EXIT_ERROR;
            break;
        }

        ChValue expanded = CH_NIL;
        ch_gc_push(&vm.gc, &expanded);
        size_t sticky_roots = push_cli_vm_roots(&vm);
        char err[256];
        if (ch_expand_toplevel(&vm, v, &expanded, err, sizeof(err)) != CH_EXPAND_OK) {
            ch_gc_pop_n(&vm.gc, sticky_roots);
            fprintf(stderr, "fmt error: %s\n", err);
            ch_gc_pop_n(&vm.gc, 2);
            rc = CH_EXIT_ERROR;
            break;
        }
        ch_gc_pop_n(&vm.gc, sticky_roots);
        if (expanded != CH_VOID) {
            ch_print_value(mem, expanded, false);
            fputc('\n', mem);
        }
        ch_gc_pop_n(&vm.gc, 2);
    }
    fclose(mem);

    if (rc == CH_EXIT_OK) {
        if (opts->flag_fmt_check) {
            size_t compare_len = src_len;
            while (compare_len > 0 && (src[compare_len - 1] == '\n' || src[compare_len - 1] == '\r')) {
                compare_len--;
            }
            size_t formatted_len = out_len;
            while (formatted_len > 0 &&
                   (out_buf[formatted_len - 1] == '\n' || out_buf[formatted_len - 1] == '\r')) {
                formatted_len--;
            }
            if (compare_len != formatted_len || memcmp(src, out_buf, compare_len) != 0) {
                fprintf(stderr, "fmt: %s would be reformatted\n", path);
                rc = CH_EXIT_ERROR;
            } else {
                printf("fmt: ok %s\n", path);
            }
        } else if (opts->output) {
            rc = write_fmt_output(opts->output, out_buf, out_len);
        } else {
            rc = write_fmt_output(path, out_buf, out_len);
            if (rc == CH_EXIT_OK) {
                printf("fmt: wrote %s\n", path);
            }
        }
    }

    free(out_buf);
    free(src);
    ch_vm_deinit(&vm);
    return rc;
}

static int cmd_check(const char *path, const ChCliOptions *opts) {
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
            fprintf(stderr, "read error: %s\n", ch_reader_error(&reader));
            ch_gc_pop(&vm.gc);
            rc = CH_EXIT_ERROR;
            break;
        }

        ChCompiler compiler;
        ch_compiler_init(&compiler, &vm);
        ChFunction *fn = NULL;
        if (ch_compile_toplevel(&compiler, v, &fn) != CH_COMPILE_OK) {
            fprintf(stderr, "check error: %s\n", ch_compiler_error(&compiler));
            ch_gc_pop(&vm.gc);
            rc = CH_EXIT_ERROR;
            break;
        }
        (void)fn;
        forms++;
        ch_gc_pop(&vm.gc);
    }

    if (rc == CH_EXIT_OK) {
        printf("check: ok (%zu form%s)\n", forms, forms == 1 ? "" : "s");
    }

    free(src);
    ch_vm_deinit(&vm);
    return rc;
}

static int run_file(ChVM *vm, const char *path) {
    size_t len = 0;
    char *src = ch_read_file(path, &len);
    if (!src) {
        fprintf(stderr, "Error opening file '%s'\n", path);
        return CH_EXIT_ERROR;
    }
    int rc = ch_eval_source(vm, src, len, 0);
    free(src);
    return rc;
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

int ch_cli_dispatch(ChCliOptions *opts, int argc, char **argv) {
    (void)argc;
    const char *argv0 = argv[0];

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

    if (opts->flag_native) {
        if (opts->command != CH_CMD_RUN) {
            fprintf(stderr, "chaaya: --native is only supported for file execution\n");
            return CH_EXIT_USAGE;
        }
        if (!opts->file) {
            fprintf(stderr, "chaaya: --native requires a Scheme file argument\n");
            usage_hint(argv0);
            return CH_EXIT_USAGE;
        }
        return ch_llvm_backend_run_file(opts->file);
    }

    /* NYI flags on an otherwise valid command */
    if (opts->nyi_flag) {
        return not_implemented(opts->nyi_flag);
    }

    switch (opts->command) {
    case CH_CMD_FEATURES:
        return cmd_features(opts);
    case CH_CMD_DOCTOR:
        return cmd_doctor(opts);
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
        return not_implemented("compile");
    case CH_CMD_CHECK:
        if (!opts->file) {
            fprintf(stderr, "check: missing file\n");
            usage_hint(argv0);
            return CH_EXIT_USAGE;
        }
        return cmd_check(opts->file, opts);
    case CH_CMD_EXPLAIN:
        return not_implemented("explain");
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
        return not_implemented("ir");
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

    int rc;
    if (opts->file) {
        rc = run_file(&vm, opts->file);
    } else if (isatty(STDIN_FILENO)) {
        rc = ch_repl_run(&vm);
    } else {
        rc = run_stdin(&vm);
    }
    ch_vm_deinit(&vm);
    return rc;
}
