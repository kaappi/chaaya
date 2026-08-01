#include "chaaya/environment.h"

#include <string.h>

ChValue ch_gc_make_environment(ChGC *gc, ChEnvKind kind) {
    ChEnvironment *env = (ChEnvironment *)ch_gc_alloc(gc, sizeof(ChEnvironment), CH_TAG_ENVIRONMENT);
    env->kind = kind;
    memset(&env->env, 0, sizeof(env->env));
    return ch_make_pointer(&env->header);
}

bool ch_is_environment(ChValue v) {
    return ch_is_pointer(v) && ch_to_object(v)->tag == CH_TAG_ENVIRONMENT;
}

ChEnvironment *ch_as_environment(ChValue v) {
    return (ChEnvironment *)ch_to_object(v);
}
