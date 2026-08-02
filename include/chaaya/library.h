#ifndef CHAAYA_LIBRARY_H
#define CHAAYA_LIBRARY_H

#include "chaaya/vm.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CH_LIB_MAX_EXPORTS 512
#define CH_LIB_ENV_MAX 1024
#define CH_LIB_MAX_LIBS 128
#define CH_LIB_MAX_LOADING 32
#define CH_LIB_MAX_INTERNALS 64
#define CH_ENV_LIB_BIT 0x8000u
/* Prefix on compiler/expander-synthesized symbols that must resolve to the
 * pristine %-internal snapshot, not whatever the program rebound that name
 * to (Kaappi #1856 / #1715). */
#define CH_BASE_BINDING_PREFIX "__chaaya_base__"

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
    ChLibEnv *runtime_env; /* owned; persists library locals for exported closures */
} ChLibrary;

typedef struct ChInternalBinding {
    ChSymbol *name; /* bare name, e.g. "%record-ref" */
    ChValue value;
} ChInternalBinding;

typedef struct ChLibraryRegistry {
    ChLibrary *libs[CH_LIB_MAX_LIBS];
    size_t count;
    /* Environments of replaced libraries. Closures compiled in a library's
     * begin block keep home_env pointers into its runtime_env and can outlive
     * the library (escaping via import), so a replaced env must stay alive
     * and GC-traced until the registry is torn down (Kaappi #820). */
    ChLibEnv **retired_envs;
    size_t retired_count;
    size_t retired_cap;
    /* Pristine %-prefixed internals captured at startup before any user code
     * can redefine them. Compiler-synthesized refs resolve here (#1856). */
    ChInternalBinding internals[CH_LIB_MAX_INTERNALS];
    size_t internal_count;
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
int ch_library_file_exists(ChVM *vm, const char *rel_path);

/* Push all registered library export values as GC roots; returns count pushed. */
size_t ch_library_push_gc_roots(ChVM *vm);

/* Mark registered library export values during collection. */
void ch_library_mark_gc_roots(ChVM *vm);

/* Snapshot defined %-prefixed globals into registry.internals (#1856). */
int ch_snapshot_internal_bindings(ChVM *vm);
/* Look up a pristine internal by bare name (e.g. "%length"). */
bool ch_lookup_internal_binding(ChVM *vm, const char *name, ChValue *out);
/* Intern CH_BASE_BINDING_PREFIX + name for desugared references. */
ChValue ch_base_binding_symbol(ChGC *gc, const char *name);

/* Resolve import-set into env (does not touch globals). */
int ch_import_set_into_env(ChVM *vm, ChValue set, ChLibEnv *out);

/* Merge multiple import-sets into env. */
int ch_environment_from_imports(ChVM *vm, ChValue *import_sets, int nsets, ChLibEnv *out);

/* Top-level (include ...) / (include-ci ...) / (cond-expand ...). */
int ch_handle_include(ChVM *vm, ChValue args, int fold_case);
int ch_handle_cond_expand(ChVM *vm, ChValue clauses);

/* Expression-position cond-expand: 0 = matched (*out_body = clause body),
 * 1 = no match, -1 = error. */
int ch_cond_expand_select(ChVM *vm, ChValue clauses, ChValue *out_body, char *err, size_t err_len);

#ifdef __cplusplus
}
#endif

#endif /* CHAAYA_LIBRARY_H */
