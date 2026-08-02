#ifndef CHAAYA_EXPANDER_INTERNAL_H
#define CHAAYA_EXPANDER_INTERNAL_H

#include "chaaya/expander.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CH_BIND_MAX 64
#define CH_HYG_MAX 4096
#define CH_EXPAND_DEPTH_MAX 256
/* Head-position macro chains (kaappi#1796): iterative steps, not nest depth. */
#define CH_EXPAND_STEP_MAX 10000
#define CH_ELLIPSIS_MAX 64

typedef struct ChBinding {
    ChSymbol *var;
    ChValue value; /* single match, or list of matches for ellipsis */
    int ellipsis;  /* 0 = scalar; >0 = ellipsis depth (1 = list, 2 = list-of-lists) */
} ChBinding;

typedef struct ChHygRename {
    ChSymbol *from;
    ChSymbol *to;
} ChHygRename;

typedef struct ChExpandCtx {
    ChVM *vm;
    ChTransformer *tr;
    const ChUseSiteBindingCheck *use_check;
    ChBinding binds[CH_BIND_MAX];
    int nbinds;
    ChHygRename renames[CH_HYG_MAX];
    int nrenames;
    int escape; /* inside (... <template>) ellipsis escape */
    int in_quote;
    char *err;
    size_t err_len;
} ChExpandCtx;

/* Shared between syntax-rules matching and template capture. */
int literal_index(ChTransformer *tr, ChSymbol *s);
int is_well_known(const char *base);

/* Bridges syntax-rules transformers into let-syntax / capture. */
void capture_transformer_templates(ChVM *vm, ChTransformer *tr);

/* Recursive toplevel expander (define-syntax, let-syntax, macros, …). */
ChExpandStatus expand_form(ChVM *vm, ChValue expr, ChValue *out, char *err, size_t err_len,
                           int depth);
ChExpandStatus expand_form_no_macros(ChVM *vm, ChValue expr, ChValue *out, char *err,
                                     size_t err_len, int depth);

#ifdef __cplusplus
}
#endif

#endif /* CHAAYA_EXPANDER_INTERNAL_H */
