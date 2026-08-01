#include "chaaya/cli.h"
#include "chaaya/version.h"

#include <stdio.h>
#include <string.h>

int main(void) {
    ChCliOptions opts;
    char *argv_help[] = {"chaaya", "--help", NULL};
    if (ch_cli_parse(2, argv_help, &opts) != CH_EXIT_OK || !opts.help) {
        fprintf(stderr, "parse --help failed\n");
        return 1;
    }

    char *argv_ver[] = {"chaaya", "--version", NULL};
    if (ch_cli_parse(2, argv_ver, &opts) != CH_EXIT_OK || !opts.version) {
        fprintf(stderr, "parse --version failed\n");
        return 1;
    }

    char *argv_bad[] = {"chaaya", "--not-a-real-flag", NULL};
    if (ch_cli_parse(2, argv_bad, &opts) != CH_EXIT_USAGE) {
        fprintf(stderr, "expected usage error for unknown flag\n");
        return 1;
    }

    char *argv_ast[] = {"chaaya", "ast", "x.scm", NULL};
    if (ch_cli_parse(3, argv_ast, &opts) != CH_EXIT_OK || opts.command != CH_CMD_AST ||
        !opts.file || strcmp(opts.file, "x.scm") != 0) {
        fprintf(stderr, "parse ast failed\n");
        return 1;
    }

    char *argv_expand[] = {"chaaya", "expand", "x.scm", NULL};
    if (ch_cli_parse(3, argv_expand, &opts) != CH_EXIT_OK || opts.command != CH_CMD_EXPAND ||
        !opts.file || strcmp(opts.file, "x.scm") != 0) {
        fprintf(stderr, "parse expand failed\n");
        return 1;
    }

    char *argv_check[] = {"chaaya", "check", "x.scm", NULL};
    if (ch_cli_parse(3, argv_check, &opts) != CH_EXIT_OK || opts.command != CH_CMD_CHECK ||
        !opts.file || strcmp(opts.file, "x.scm") != 0) {
        fprintf(stderr, "parse check failed\n");
        return 1;
    }

    char *argv_lsp[] = {"chaaya", "lsp", NULL};
    if (ch_cli_parse(2, argv_lsp, &opts) != CH_EXIT_OK || opts.command != CH_CMD_LSP) {
        fprintf(stderr, "parse lsp failed\n");
        return 1;
    }

    char *argv_wasm[] = {"chaaya", "wasm", NULL};
    if (ch_cli_parse(2, argv_wasm, &opts) != CH_EXIT_OK || opts.command != CH_CMD_WASM) {
        fprintf(stderr, "parse wasm failed\n");
        return 1;
    }

    char *argv_run[] = {"chaaya", "prog.scm", "a", "b", NULL};
    if (ch_cli_parse(4, argv_run, &opts) != CH_EXIT_OK || opts.command != CH_CMD_RUN ||
        opts.script_arg_count != 2) {
        fprintf(stderr, "parse script args failed\n");
        return 1;
    }

    char *argv_native[] = {"chaaya", "--native", "prog.scm", NULL};
    if (ch_cli_parse(3, argv_native, &opts) != CH_EXIT_OK || !opts.flag_native || !opts.file ||
        strcmp(opts.file, "prog.scm") != 0) {
        fprintf(stderr, "parse --native failed\n");
        return 1;
    }

    char *argv_nyi[] = {"chaaya", "--sandbox", NULL};
    if (ch_cli_parse(2, argv_nyi, &opts) != CH_EXIT_OK || !opts.nyi_flag) {
        fprintf(stderr, "expected nyi flag for --sandbox\n");
        return 1;
    }

    char *argv_feat[] = {"chaaya", "features", "--json", NULL};
    if (ch_cli_parse(3, argv_feat, &opts) != CH_EXIT_OK || opts.command != CH_CMD_FEATURES ||
        !opts.json) {
        fprintf(stderr, "parse features --json failed\n");
        return 1;
    }

    if (strcmp(CHAAYA_VERSION_BANNER, "Chaaya Scheme v0.1.0") != 0) {
        fprintf(stderr, "bad banner\n");
        return 1;
    }

    printf("ok\n");
    return 0;
}
