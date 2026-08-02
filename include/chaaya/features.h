#ifndef CHAAYA_FEATURES_H
#define CHAAYA_FEATURES_H

#include "chaaya/vm.h"

#ifdef __cplusplus
extern "C" {
#endif

/* True if `name` is in this build's feature identifiers. */
int ch_feature_present(const char *name);

/* R7RS cond-expand feature requirement (symbol | (and ...) | (or ...) |
 * (not ...) | (library name-list)). */
int ch_eval_feature_req(ChVM *vm, ChValue req);

/* Build the (features) list (newly consed). */
ChValue ch_features_list(ChVM *vm);

void ch_features_print_text(FILE *out);
int ch_features_print_json(FILE *out, const char **lib_paths, size_t lib_path_count);

void ch_register_features_primitives(ChVM *vm);

#ifdef __cplusplus
}
#endif

#endif /* CHAAYA_FEATURES_H */
