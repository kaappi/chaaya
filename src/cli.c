#include "chaaya/cli.h"

#include "chaaya/eval.h"
#include "chaaya/opcode.h"
#include "chaaya/printer.h"
#include "chaaya/reader.h"
#include "chaaya/repl.h"
#include "chaaya/version.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

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
    printf("  fmt [files...]     Canonically format Scheme in place; --check\n");
    printf("  cache status       Show the bytecode cache location and entries\n");
    printf("  cache clear        Remove all bytecode cache entries\n");
    printf("\n");
    printf("Options:\n");
    printf("  -h, --help         Show this help message\n");
    printf("  --version          Show version\n");
    printf("  -v                 Show version (alias)\n");
    printf("  --lib-path <path>  Add library search path (repeatable)\n");
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
           strcmp(s, "fmt") == 0 || strcmp(s, "cache") == 0;
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
            /* fmt --check */
            mark_nyi(out, "--check");
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

static void print_completions(const char *shell) {
    if (strcmp(shell, "bash") == 0) {
        puts("# chaaya bash completion");
        puts("_chaaya() {");
        puts("  local cur=\"${COMP_WORDS[COMP_CWORD]}\"");
        puts("  local cmds=\"compile check explain features test ast expand ir doctor fmt cache\"");
        puts("  local opts=\"--help --version --lib-path --completions --compile --emit-llvm -o "
             "--disassemble --sandbox --gc-stats --profile --timings --coverage --json\"");
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
        puts("  '--completions[shell completions]:shell:(bash zsh fish)' \\");
        puts("  '1:command:(compile check explain features test ast expand ir doctor fmt cache)'");
        return;
    }
    if (strcmp(shell, "fish") == 0) {
        puts("complete -c chaaya -s h -l help -d 'Show help'");
        puts("complete -c chaaya -l version -d 'Show version'");
        puts("complete -c chaaya -l lib-path -r -d 'Library search path'");
        puts("complete -c chaaya -l completions -xa 'bash zsh fish'");
        puts("complete -c chaaya -a 'compile check explain features test ast expand ir doctor fmt "
             "cache'");
        return;
    }
    fprintf(stderr, "unknown shell for --completions: %s (want bash, zsh, or fish)\n", shell);
}

static int cmd_features(int json) {
    if (json) {
        printf("{\n");
        printf("  \"implementation\": \"chaaya\",\n");
        printf("  \"version\": \"%s\",\n", CHAAYA_VERSION);
        printf("  \"language\": \"C17\",\n");
#ifdef CHAAYA_HAS_LINENOISE
        printf("  \"linenoise\": true,\n");
#else
        printf("  \"linenoise\": false,\n");
#endif
        printf("  \"opcodes\": %d,\n", (int)CH_OP_HALT + 1);
        printf("  \"stage\": \"bootstrap\",\n");
        printf("  \"r7rs_small\": false,\n");
        printf("  \"libraries\": false,\n");
        printf("  \"macros\": false,\n");
        printf("  \"native_backend\": false\n");
        printf("}\n");
    } else {
        printf("%s\n", CHAAYA_VERSION_BANNER);
        printf("language:     C17\n");
#ifdef CHAAYA_HAS_LINENOISE
        printf("linenoise:    yes\n");
#else
        printf("linenoise:    no\n");
#endif
        printf("opcodes:      %d\n", (int)CH_OP_HALT + 1);
        printf("stage:        bootstrap\n");
        printf("r7rs-small:   not yet\n");
        printf("libraries:    not yet\n");
        printf("macros:       not yet\n");
        printf("native:       not yet\n");
    }
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
        ChReadStatus st = ch_read_datum(&reader, &v);
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

static int cmd_cache_status(void) {
    printf("Bytecode cache: not implemented yet (bootstrap)\n");
    printf("Future location: ~/.chaaya/cache (or $CHAAYA_HOME/cache)\n");
    printf("Entries: 0\n");
    return CH_EXIT_OK;
}

static int cmd_cache_clear(void) {
    printf("Bytecode cache: nothing to clear (not implemented yet)\n");
    return CH_EXIT_OK;
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

    /* NYI flags on an otherwise valid command */
    if (opts->nyi_flag) {
        return not_implemented(opts->nyi_flag);
    }

    switch (opts->command) {
    case CH_CMD_FEATURES:
        return cmd_features(opts->json);
    case CH_CMD_DOCTOR:
        return cmd_doctor(opts);
    case CH_CMD_AST:
        if (!opts->file) {
            fprintf(stderr, "ast: missing file\n");
            usage_hint(argv0);
            return CH_EXIT_USAGE;
        }
        return cmd_ast(opts->file);
    case CH_CMD_CACHE_STATUS:
        return cmd_cache_status();
    case CH_CMD_CACHE_CLEAR:
        return cmd_cache_clear();
    case CH_CMD_COMPILE:
        return not_implemented("compile");
    case CH_CMD_CHECK:
        return not_implemented("check");
    case CH_CMD_EXPLAIN:
        return not_implemented("explain");
    case CH_CMD_TEST:
        return not_implemented("test");
    case CH_CMD_EXPAND:
        return not_implemented("expand");
    case CH_CMD_IR:
        return not_implemented("ir");
    case CH_CMD_FMT:
        return not_implemented("fmt");
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
