#include "chaaya/cli.h"
#include "cli_internal.h"

#include "chaaya/version.h"

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void usage_hint(const char *argv0) {
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
    printf("       %s test [--json] [-j N] [--seed N] [--changed] [paths...]\n", argv0);
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
    printf("  explain <code>     Explain a diagnostic code (e.g. CH3001); --all\n");
    printf("  features           Report this build's capabilities; --json\n");
    printf("  test [paths...]    Run SRFI-64 suites; --json/--seed/-j/--changed\n");
    printf("  ast <file>         Print post-read datums (read + write)\n");
    printf("  expand <file>      Print the program after full macro expansion\n");
    printf("  ir <file> [--no-opt]  Print the IR tree\n");
    printf("  doctor [--json]    Check the installation and environment\n");
    printf("  lsp                Run a minimal stdio JSON-RPC loop\n");
    printf("  wasm               WebAssembly backend helper/build entrypoint\n");
    printf("  fmt [files...]     Canonically format Scheme in place; --check\n");
    printf("  cache status       Show the bytecode cache location and entries\n");
    printf("  cache clear        Remove all bytecode cache entries\n");
    printf("\n");
    printf("Options:\n");
    printf("  -h, --help         Show this help message\n");
    printf("  --version          Show version\n");
    printf("  -v                 Show version (alias)\n");
    printf("  --lib-path <path>  Add library search path (repeatable)\n");
    printf("  --native           Route file execution through LLVM backend\n");
    printf("  --compile          Compile file to bytecode (.chbc)\n");
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
    printf("  -j, --jobs <n>     (test) Run up to N tests in parallel\n");
    printf("  --seed <n>         (test) Set and report deterministic test seed\n");
    printf("  --changed          (test) Run suites affected by git changes\n");
    printf("  --list-affected    (test) List affected suites without running\n");
    printf("  --since <rev>      (test) Git revision for --changed (default HEAD)\n");
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

static int parse_long_arg(const char *flag, const char *text, long *out) {
    char *end = NULL;
    errno = 0;
    long v = strtol(text, &end, 10);
    if (errno != 0 || !end || *end != '\0') {
        fprintf(stderr, "%s expects an integer value\n", flag);
        return CH_EXIT_USAGE;
    }
    *out = v;
    return CH_EXIT_OK;
}

static int parse_size_arg(const char *flag, const char *text, size_t *out) {
    char *end = NULL;
    errno = 0;
    unsigned long long v = strtoull(text, &end, 10);
    if (errno != 0 || !end || *end != '\0' || v > SIZE_MAX) {
        fprintf(stderr, "%s expects a non-negative integer value\n", flag);
        return CH_EXIT_USAGE;
    }
    *out = (size_t)v;
    return CH_EXIT_OK;
}

static int parse_int_arg(const char *flag, const char *text, int *out) {
    long v = 0;
    int rc = parse_long_arg(flag, text, &v);
    if (rc != CH_EXIT_OK) {
        return rc;
    }
    if (v < INT_MIN || v > INT_MAX) {
        fprintf(stderr, "%s value out of range\n", flag);
        return CH_EXIT_USAGE;
    }
    *out = (int)v;
    return CH_EXIT_OK;
}

static int parse_uint_arg(const char *flag, const char *text, unsigned *out) {
    char *end = NULL;
    errno = 0;
    unsigned long v = strtoul(text, &end, 10);
    if (errno != 0 || !end || *end != '\0' || v > UINT_MAX) {
        fprintf(stderr, "%s expects a non-negative integer value\n", flag);
        return CH_EXIT_USAGE;
    }
    *out = (unsigned)v;
    return CH_EXIT_OK;
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
            if (out->command == CH_CMD_TEST) {
                out->test_json = 1;
            } else {
                out->json = 1;
            }
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
            continue;
        }
        if (strcmp(a, "--emit-llvm") == 0) {
            out->flag_emit_llvm = 1;
            continue;
        }
        if (strcmp(a, "--disassemble") == 0) {
            out->flag_disassemble = 1;
            continue;
        }
        if (strcmp(a, "--sandbox") == 0) {
            out->flag_sandbox = 1;
            continue;
        }
        if (strcmp(a, "--gc-stats") == 0) {
            out->flag_gc_stats = 1;
            continue;
        }
        if (strcmp(a, "--profile") == 0) {
            out->flag_profile = 1;
            continue;
        }
        if (strcmp(a, "--coverage") == 0) {
            out->flag_coverage = 1;
            continue;
        }
        if (strcmp(a, "--no-ir-opt") == 0 || strcmp(a, "--no-opt") == 0) {
            out->flag_no_ir_opt = 1;
            continue;
        }
        if (strcmp(a, "--deny-warnings") == 0) {
            out->flag_deny_warnings = 1;
            continue;
        }
        if (strcmp(a, "--timings") == 0 || strncmp(a, "--timings=", 10) == 0) {
            out->flag_timings = 1;
            out->timings_json = 0;
            if (strncmp(a, "--timings=", 10) == 0) {
                const char *fmt = a + 10;
                if (strcmp(fmt, "json") == 0) {
                    out->timings_json = 1;
                } else if (strcmp(fmt, "text") != 0) {
                    fprintf(stderr, "--timings expects text or json\n");
                    usage_hint(argv0);
                    return CH_EXIT_USAGE;
                }
            }
            continue;
        }
        if (strncmp(a, "--diagnostics=", 14) == 0) {
            const char *fmt = a + 14;
            out->flag_diagnostics = 1;
            if (strcmp(fmt, "json") == 0) {
                out->diagnostics_format = CH_DIAG_FMT_JSON;
            } else if (strcmp(fmt, "text") == 0) {
                out->diagnostics_format = CH_DIAG_FMT_TEXT;
            } else {
                fprintf(stderr, "--diagnostics expects text or json\n");
                usage_hint(argv0);
                return CH_EXIT_USAGE;
            }
            continue;
        }
        if (strcmp(a, "--timeout") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "--timeout requires a value\n");
                return CH_EXIT_USAGE;
            }
            out->flag_timeout = 1;
            if (parse_long_arg("--timeout", argv[++i], &out->timeout_ms) != CH_EXIT_OK) {
                return CH_EXIT_USAGE;
            }
            if (out->timeout_ms < 0) {
                fprintf(stderr, "--timeout expects a non-negative value\n");
                return CH_EXIT_USAGE;
            }
            continue;
        }
        if (strcmp(a, "--max-memory") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "--max-memory requires a value\n");
                return CH_EXIT_USAGE;
            }
            out->flag_max_memory = 1;
            if (parse_size_arg("--max-memory", argv[++i], &out->max_memory_bytes) != CH_EXIT_OK) {
                return CH_EXIT_USAGE;
            }
            continue;
        }
        if (strcmp(a, "--profile-json") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "--profile-json requires a path\n");
                return CH_EXIT_USAGE;
            }
            out->flag_profile_json = 1;
            out->profile_json_path = argv[++i];
            continue;
        }
        if (strcmp(a, "--coverage-xml") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "--coverage-xml requires a path\n");
                return CH_EXIT_USAGE;
            }
            out->flag_coverage_xml = 1;
            out->coverage_xml_path = argv[++i];
            continue;
        }
        if ((strcmp(a, "-j") == 0 || strcmp(a, "--jobs") == 0) && out->command == CH_CMD_TEST) {
            if (i + 1 >= argc) {
                fprintf(stderr, "%s requires a value\n", a);
                return CH_EXIT_USAGE;
            }
            if (parse_int_arg(a, argv[++i], &out->test_jobs) != CH_EXIT_OK || out->test_jobs <= 0) {
                fprintf(stderr, "%s expects a positive integer\n", a);
                return CH_EXIT_USAGE;
            }
            continue;
        }
        if (strcmp(a, "--seed") == 0 && out->command == CH_CMD_TEST) {
            if (i + 1 >= argc) {
                fprintf(stderr, "--seed requires a value\n");
                return CH_EXIT_USAGE;
            }
            if (parse_uint_arg("--seed", argv[++i], &out->test_seed) != CH_EXIT_OK) {
                return CH_EXIT_USAGE;
            }
            out->test_seed_set = 1;
            continue;
        }
        if (strcmp(a, "--changed") == 0 && out->command == CH_CMD_TEST) {
            out->test_changed = 1;
            continue;
        }
        if (strcmp(a, "--list-affected") == 0 && out->command == CH_CMD_TEST) {
            out->test_list_affected = 1;
            continue;
        }
        if (strcmp(a, "--since") == 0 && out->command == CH_CMD_TEST) {
            if (i + 1 >= argc) {
                fprintf(stderr, "--since requires a revision\n");
                return CH_EXIT_USAGE;
            }
            out->test_since = argv[++i];
            continue;
        }
        if (strcmp(a, "--all") == 0 && out->command == CH_CMD_EXPLAIN) {
            out->explain_all = 1;
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

void print_completions(const char *shell) {
    if (strcmp(shell, "bash") == 0) {
        puts("# chaaya bash completion");
        puts("_chaaya() {");
        puts("  local cur=\"${COMP_WORDS[COMP_CWORD]}\"");
        puts("  local cmds=\"compile check explain features test ast expand ir doctor lsp wasm fmt "
             "cache\"");
        puts("  local opts=\"--help --version --lib-path --native --completions --compile "
             "--emit-llvm -o --disassemble --sandbox --gc-stats --profile --profile-json "
             "--timings --coverage --coverage-xml --timeout --max-memory --json --jobs --seed\"");
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
        puts("  '--native[run via LLVM backend]' \\");
        puts("  '--compile[compile file to bytecode cache/blob]' \\");
        puts("  '--completions[shell completions]:shell:(bash zsh fish)' \\");
        puts("  '1:command:(compile check explain features test ast expand ir doctor lsp wasm fmt "
             "cache)'");
        return;
    }
    if (strcmp(shell, "fish") == 0) {
        puts("complete -c chaaya -s h -l help -d 'Show help'");
        puts("complete -c chaaya -l version -d 'Show version'");
        puts("complete -c chaaya -l lib-path -r -d 'Library search path'");
        puts("complete -c chaaya -l native -d 'Run via LLVM backend'");
        puts("complete -c chaaya -l compile -d 'Compile file to bytecode'");
        puts("complete -c chaaya -l completions -xa 'bash zsh fish'");
        puts("complete -c chaaya -a 'compile check explain features test ast expand ir doctor lsp "
             "wasm fmt cache'");
        return;
    }
    fprintf(stderr, "unknown shell for --completions: %s (want bash, zsh, or fish)\n", shell);
}
