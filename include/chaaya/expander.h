#ifndef CHAAYA_EXPANDER_H
#define CHAAYA_EXPANDER_H

#include "chaaya/vm.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum ChExpandStatus {
    CH_EXPAND_OK = 0,
    CH_EXPAND_ERROR,
} ChExpandStatus;

#define CH_LITERAL_UNBOUND 0u
#define CH_LITERAL_GLOBAL_BASE 0x80000000u
#define CH_LITERAL_LOCAL_BASE 0x40000000u

typedef struct ChUseSiteBindingCheck {
    const void *ctx;
    uint32_t (*resolve)(const void *ctx, const char *name);
} ChUseSiteBindingCheck;

/* Parse (syntax-rules (lit ...) (pat tmpl) ...) into a transformer. */
ChExpandStatus ch_parse_syntax_rules(ChVM *vm, ChValue spec, ChTransformer **out, char *err,
                                     size_t err_len);

/* Expand one macro use: (keyword . args) using transformer. */
ChExpandStatus ch_expand_macro(ChVM *vm, ChTransformer *tr, ChValue use, ChValue *out, char *err,
                               size_t err_len);

ChExpandStatus ch_expand_macro_checked(ChVM *vm, ChTransformer *tr, ChValue use,
                                       const ChUseSiteBindingCheck *use_check, ChValue *out,
                                       char *err, size_t err_len);

/* Lookup define-syntax binding. */
ChTransformer *ch_vm_lookup_macro(ChVM *vm, ChSymbol *name);
int ch_vm_define_macro(ChVM *vm, ChSymbol *name, ChTransformer *tr);

/* Fully expand macros in expr (for CLI expand). */
ChExpandStatus ch_expand_toplevel(ChVM *vm, ChValue expr, ChValue *out, char *err, size_t err_len);

#ifdef __cplusplus
}
#endif

#endif /* CHAAYA_EXPANDER_H */
