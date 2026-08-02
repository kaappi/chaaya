#ifndef CHAAYA_COMPILER_H
#define CHAAYA_COMPILER_H

#include "chaaya/diagnostics.h"
#include "chaaya/vm.h"
#include "chaaya/value.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum ChCompileStatus {
    CH_COMPILE_OK = 0,
    CH_COMPILE_ERROR,
} ChCompileStatus;

typedef struct ChCompiler {
    ChVM *vm;
    char error[256];
    ChDiagCode error_code;
    int error_line;
    int error_column;
    uint32_t next_binding_id;
} ChCompiler;

void ch_compiler_init(ChCompiler *c, ChVM *vm);
void ch_compiler_set_location(ChCompiler *c, int line, int column);
ChCompileStatus ch_compile_toplevel(ChCompiler *c, ChValue expr, ChFunction **out_fn);
const char *ch_compiler_error(const ChCompiler *c);
ChDiagCode ch_compiler_error_code(const ChCompiler *c);

#ifdef __cplusplus
}
#endif

#endif /* CHAAYA_COMPILER_H */
