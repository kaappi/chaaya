#ifndef CHAAYA_HASHTABLE_H
#define CHAAYA_HASHTABLE_H

#include "chaaya/value.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum ChHashtableStatus {
    CH_HASHTABLE_OK = 0,
    CH_HASHTABLE_NOT_FOUND = 1,
    CH_HASHTABLE_BAD_KEY = 2,
    CH_HASHTABLE_OOM = 3,
} ChHashtableStatus;

bool ch_hashtable_key_supported(ChValue key);

ChHashtableStatus ch_hashtable_get(const ChHashtable *ht, ChValue key, ChValue *out_value);
ChHashtableStatus ch_hashtable_set(ChHashtable *ht, ChValue key, ChValue value);
ChHashtableStatus ch_hashtable_delete(ChHashtable *ht, ChValue key);

#ifdef __cplusplus
}
#endif

#endif /* CHAAYA_HASHTABLE_H */
