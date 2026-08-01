#ifndef CHAAYA_ENVIRONMENT_H
#define CHAAYA_ENVIRONMENT_H

#include "chaaya/library.h"
#include "chaaya/value.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum ChEnvKind {
    CH_ENV_CUSTOM = 0,
    CH_ENV_INTERACTION = 1,
    CH_ENV_NULL = 2,
    CH_ENV_REPORT = 3,
} ChEnvKind;

typedef struct ChEnvironment {
    ChObject header;
    ChEnvKind kind;
    ChLibEnv env;
} ChEnvironment;

ChValue ch_gc_make_environment(ChGC *gc, ChEnvKind kind);
bool ch_is_environment(ChValue v);
ChEnvironment *ch_as_environment(ChValue v);

/* Populate env from R7RS import-set arguments (one per arg). */
int ch_environment_from_imports(ChVM *vm, ChValue *import_sets, int nsets, ChLibEnv *out);

#ifdef __cplusplus
}
#endif

#endif /* CHAAYA_ENVIRONMENT_H */
