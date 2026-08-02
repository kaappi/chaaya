#ifndef CHAAYA_GC_DEEP_COPY_H
#define CHAAYA_GC_DEEP_COPY_H

#include "chaaya/gc.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Cycle-safe deep copy from any heap into *dest*. Immediates and fixnums/flonums
 * are returned as-is. Symbols are interned (or uninterned-copied) on dest.
 * Returns CH_UNDEFINED on error; sets dest->vm->error when dest->vm is non-NULL. */
ChValue ch_gc_deep_copy(ChGC *dest, ChValue src);

#ifdef __cplusplus
}
#endif

#endif /* CHAAYA_GC_DEEP_COPY_H */
