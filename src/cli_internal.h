#ifndef CHAAYA_CLI_INTERNAL_H
#define CHAAYA_CLI_INTERNAL_H

#include "chaaya/cli.h"

#include "chaaya/compiler.h"
#include "chaaya/reader.h"
#include "chaaya/vm.h"

#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

void usage_hint(const char *argv0);
void print_completions(const char *shell);
int not_implemented(const char *what);

ChReadStatus read_cli_datum(ChVM *vm, ChReader *reader, ChValue *out);
void report_reader_diag(const char *path, const char *src, size_t len, const ChReader *reader);
void report_compiler_diag(const char *path, const ChCompiler *compiler);

int cmd_test(const char *argv0, ChCliOptions *opts);
int cmd_wasm_stub(void);
int cmd_features(ChCliOptions *opts);
int cmd_ast(const char *path);
int cmd_expand(const char *path, const ChCliOptions *opts);
int cmd_fmt(const char *path, const ChCliOptions *opts);
int cmd_check(const char *path, const ChCliOptions *opts);
int cmd_ir(const char *path, const ChCliOptions *opts);

#ifdef __cplusplus
}
#endif

#endif /* CHAAYA_CLI_INTERNAL_H */
