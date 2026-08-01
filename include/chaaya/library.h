#ifndef CHAAYA_LIBRARY_H
#define CHAAYA_LIBRARY_H

#include "chaaya/vm.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CH_LIB_MAX_EXPORTS 512
#define CH_LIB_ENV_MAX 512
#define CH_LIB_MAX_LIBS 128
#define CH_LIB_MAX_LOADING 32
#define CH_ENV_LIB_BIT 0x8000u

typedef struct ChLibBinding {
    ChSymbol *name;
    ChValue value;
    bool defined;
} ChLibBinding;

typedef struct ChLibEnv {
    ChLibBinding bindings[CH_LIB_ENV_MAX];
    size_t count;
} ChLibEnv;

typedef struct ChLibrary {
    char *name; /* owned, e.g. "scheme.base" */
    ChSymbol *export_names[CH_LIB_MAX_EXPORTS];
    ChValue export_values[CH_LIB_MAX_EXPORTS];
    size_t export_count;
} ChLibrary;

typedef struct ChLibraryRegistry {
    ChLibrary *libs[CH_LIB_MAX_LIBS];
    size_t count;
} ChLibraryRegistry;

void ch_library_registry_init(ChLibraryRegistry *reg);
void ch_library_registry_deinit(ChLibraryRegistry *reg);

ChLibrary *ch_library_lookup(ChLibraryRegistry *reg, const char *name);
int ch_library_register(ChLibraryRegistry *reg, ChLibrary *lib); /* takes ownership */

/* Build dotted name "a.b.c" from (a b c); caller frees. */
char *ch_library_name_to_string(ChValue name_list);
/* Build relative path "a/b/c.sld"; caller frees. */
char *ch_library_name_to_path(ChValue name_list);

int ch_lib_env_intern(ChLibEnv *env, ChSymbol *sym);
void ch_lib_env_define(ChLibEnv *env, int idx, ChValue v);
int ch_lib_env_find(const ChLibEnv *env, ChSymbol *sym);

/* Register built-in (scheme …) libraries from current globals. */
int ch_register_scheme_base_library(ChVM *vm);
int ch_register_builtin_libraries(ChVM *vm);

/* Top-level (import ...) and (define-library ...). Return 0 on success. */
int ch_handle_import(ChVM *vm, ChValue args);
int ch_handle_define_library(ChVM *vm, ChValue args);

/* Dispatch one top-level form (import / define-library / expression). */
int ch_eval_toplevel_form(ChVM *vm, ChValue expr);

/* Resolve and load a library by name list; returns library or NULL. */
ChLibrary *ch_ensure_library(ChVM *vm, ChValue name_list);

/* Push all registered library export values as GC roots; returns count pushed. */
size_t ch_library_push_gc_roots(ChVM *vm);

/* True if a relative .sld path resolves on disk (does not load). */
int ch_library_file_exists(ChVM *vm, const char *rel_path);

/* Top-level (include ...) / (include-ci ...) / (cond-expand ...). */
int ch_handle_include(ChVM *vm, ChValue args, int fold_case);
int ch_handle_cond_expand(ChVM *vm, ChValue clauses);

#ifdef __cplusplus
}
#endif

#endif /* CHAAYA_LIBRARY_H */
