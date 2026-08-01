#ifndef CHAAYA_REPL_H
#define CHAAYA_REPL_H

#include "chaaya/vm.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Interactive or plain REPL. Returns process exit code. */
int ch_repl_run(ChVM *vm);

/* Paren depth outside strings/line-comments (for multiline / tests). */
int ch_repl_paren_depth(const char *s);

#ifdef __cplusplus
}
#endif

#endif /* CHAAYA_REPL_H */
