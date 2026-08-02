#ifndef CHAAYA_CACHE_H
#define CHAAYA_CACHE_H

#include "chaaya/vm.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bytecode cache CLI helpers.
 * Return process-style exit code: 0 success, 1 error. */
int ch_cache_status(void);
int ch_cache_clear(void);

/* Auto-cache for plain file runs. Returns 1 if loaded; caller frees fns array only. */
int ch_cache_try_load(ChVM *vm, const char *path, const char *source, size_t source_len,
                      ChFunction ***out_fns, size_t *out_count);

/* Store compiled top-level functions for path/source. Returns 0 on success. */
int ch_cache_store(ChVM *vm, const char *path, const char *source, size_t source_len,
                   ChFunction **fns, size_t count);

#ifdef __cplusplus
}
#endif

#endif /* CHAAYA_CACHE_H */
