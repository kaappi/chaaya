#ifndef CHAAYA_CACHE_H
#define CHAAYA_CACHE_H

#ifdef __cplusplus
extern "C" {
#endif

/* Bytecode cache CLI helpers.
 * Return process-style exit code: 0 success, 1 error. */
int ch_cache_status(void);
int ch_cache_clear(void);

#ifdef __cplusplus
}
#endif

#endif /* CHAAYA_CACHE_H */
