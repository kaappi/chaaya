#ifndef CHAAYA_RUNTIME_EXPORTS_H
#define CHAAYA_RUNTIME_EXPORTS_H

/*
 * C-ABI bridge for LLVM-native binaries. Values are NaN-boxed uint64_t (ChValue).
 * Independent of Kaappi's kaappi_* export names.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ChVM ChVM;

/* Lifecycle */
ChVM *ch_rt_init(void);
void ch_rt_deinit(ChVM *vm);
void ch_rt_set_argv(ChVM *vm, char **argv);

/* Globals */
uint64_t ch_rt_global_lookup(ChVM *vm, const char *name, uint64_t name_len);
void ch_rt_define_global(ChVM *vm, const char *name, uint64_t name_len, uint64_t value);
void ch_rt_set_global(ChVM *vm, const char *name, uint64_t name_len, uint64_t value);

/* Eval / call */
uint64_t ch_rt_eval(ChVM *vm, const char *src, uint64_t src_len);
uint64_t ch_rt_eval_cached(ChVM *vm, const char *src, uint64_t src_len, uint64_t *slot);
uint64_t ch_rt_quote_cached(ChVM *vm, const char *src, uint64_t src_len, uint64_t *slot);
uint64_t ch_rt_call_scheme(ChVM *vm, uint64_t proc, uint64_t *args, uint64_t nargs);
uint64_t ch_rt_apply(ChVM *vm, uint64_t proc, uint64_t args_list);

/* Heap helpers */
uint64_t ch_rt_cons(ChVM *vm, uint64_t car, uint64_t cdr);
uint64_t ch_rt_car(uint64_t pair);
uint64_t ch_rt_cdr(uint64_t pair);
uint64_t ch_rt_make_string(ChVM *vm, const char *bytes, uint64_t len);
uint64_t ch_rt_intern_symbol(ChVM *vm, const char *name, uint64_t name_len);

/* Fixnum fast paths (fall back to Scheme primitives on overflow / non-fixnum) */
uint64_t ch_rt_fixnum_add(uint64_t a, uint64_t b);
uint64_t ch_rt_fixnum_sub(uint64_t a, uint64_t b);
uint64_t ch_rt_fixnum_mul(uint64_t a, uint64_t b);
uint64_t ch_rt_fixnum_lt(uint64_t a, uint64_t b);
uint64_t ch_rt_fixnum_eq(uint64_t a, uint64_t b);

/* Program entry used by generated @main when full native lowering is unavailable */
int ch_rt_main(void);
int ch_rt_run_source(const char *src, size_t len);
int ch_rt_run_file(const char *path);
void ch_rt_set_embedded_source(const char *src, size_t len);

/* Host arch gate for doctor / compile */
int ch_rt_native_arch_supported(void);

#ifdef __cplusplus
}
#endif

#endif /* CHAAYA_RUNTIME_EXPORTS_H */
