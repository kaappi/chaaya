#ifndef CHAAYA_CLI_H
#define CHAAYA_CLI_H

#include "chaaya/vm.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CH_EXIT_OK 0
#define CH_EXIT_ERROR 1
#define CH_EXIT_USAGE 2

typedef enum ChCliCommand {
    CH_CMD_RUN = 0, /* default: REPL / file / stdin */
    CH_CMD_COMPILE,
    CH_CMD_CHECK,
    CH_CMD_EXPLAIN,
    CH_CMD_FEATURES,
    CH_CMD_TEST,
    CH_CMD_AST,
    CH_CMD_EXPAND,
    CH_CMD_IR,
    CH_CMD_DOCTOR,
    CH_CMD_FMT,
    CH_CMD_CACHE_STATUS,
    CH_CMD_CACHE_CLEAR,
    CH_CMD_LSP,
    CH_CMD_WASM,
} ChCliCommand;

typedef struct ChCliOptions {
    ChCliCommand command;
    int help;
    int version;
    int json; /* features / doctor */
    const char *completions_shell; /* bash|zsh|fish or NULL */
    const char *file;              /* primary file for run/ast/... */
    const char *output;            /* -o */
    const char *explain_code;
    const char *lib_paths[CH_VM_MAX_LIB_PATHS];
    size_t lib_path_count;
    const char *script_args[CH_VM_MAX_SCRIPT_ARGS];
    size_t script_arg_count;
    /* Flags accepted but not implemented (set → exit with message) */
    int flag_native;
    int flag_compile;
    int flag_emit_llvm;
    int flag_disassemble;
    int flag_sandbox;
    int flag_gc_stats;
    int flag_profile;
    int flag_coverage;
    int flag_no_ir_opt;
    int flag_deny_warnings;
    int flag_timings;
    int flag_diagnostics;
    int flag_timeout;
    int flag_max_memory;
    int flag_profile_json;
    int flag_coverage_xml;
    int flag_fmt_check; /* fmt --check */
    const char *nyi_flag; /* first not-yet-implemented flag name seen */
} ChCliOptions;

void ch_cli_print_help(const char *argv0);
void ch_cli_print_version(void);

/* Parse argv. Returns CH_EXIT_OK on success, CH_EXIT_USAGE on parse error
 * (message already printed). Does not execute. */
int ch_cli_parse(int argc, char **argv, ChCliOptions *out);

/* Run after successful parse. Returns process exit code. */
int ch_cli_dispatch(ChCliOptions *opts, int argc, char **argv);

#ifdef __cplusplus
}
#endif

#endif /* CHAAYA_CLI_H */
