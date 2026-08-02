#ifndef CHAAYA_FMT_H
#define CHAAYA_FMT_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Format Scheme source. On success returns 0 and sets *out_text (malloc'd;
 * caller frees) and *out_len. On failure returns -1 and may set *err_out. */
int ch_fmt_source(const char *src, size_t len, char **out_text, size_t *out_len, char *err_out,
                  size_t err_len);

/* Format a file in place, or --check mode. Returns process exit code. */
int ch_fmt_file(const char *path, int check_only, const char *output_path);

#ifdef __cplusplus
}
#endif

#endif /* CHAAYA_FMT_H */
