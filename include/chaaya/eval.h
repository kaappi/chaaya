#ifndef CHAAYA_EVAL_H
#define CHAAYA_EVAL_H

#include "chaaya/vm.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Evaluate one or more top-level forms from source.
 * print_results: print non-void values; bind `_` to last non-void result. */
int ch_eval_source(ChVM *vm, const char *source, size_t len, int print_results);

/* Evaluate a single datum; optional environment (NULL = interaction/globals). */
int ch_eval_datum(ChVM *vm, ChValue expr, ChValue env_or_void, ChValue *out);

/* Read and evaluate all forms from a file; optional environment. */
int ch_eval_file(ChVM *vm, const char *path, ChValue env_or_void, ChValue *last_out);

char *ch_read_file(const char *path, size_t *out_len); /* malloc'd; caller frees */

/* Bind a value to the global named by cstr (creates if needed). */
void ch_vm_set_global_cstr(ChVM *vm, const char *name, ChValue v);

/* Human-readable type name for ,type */
const char *ch_value_type_name(ChValue v);

#ifdef __cplusplus
}
#endif

#endif /* CHAAYA_EVAL_H */
