#include "chaaya/features.h"

#include "chaaya/library.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const k_features[] = {
    "r7rs",
    "chaaya",
    "ieee-float",
    "exact-closed",
#if defined(__APPLE__)
    "darwin",
    "macos",
    "posix",
#elif defined(__linux__)
    "linux",
    "posix",
#elif defined(_WIN32)
    "windows",
#else
    "posix",
#endif
#if defined(__LITTLE_ENDIAN__) || defined(_LITTLE_ENDIAN) ||                                        \
    (defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
    "little-endian",
#endif
};

int ch_feature_present(const char *name) {
    for (size_t i = 0; i < sizeof(k_features) / sizeof(k_features[0]); i++) {
        if (strcmp(k_features[i], name) == 0) {
            return 1;
        }
    }
    return 0;
}

static int library_available(ChVM *vm, ChValue name_list) {
    char *dotted = ch_library_name_to_string(name_list);
    if (!dotted) {
        return 0;
    }
    if (vm->libraries && ch_library_lookup(vm->libraries, dotted)) {
        free(dotted);
        return 1;
    }
    free(dotted);
    char *rel = ch_library_name_to_path(name_list);
    if (!rel) {
        return 0;
    }
    int ok = ch_library_file_exists(vm, rel);
    free(rel);
    return ok;
}

int ch_eval_feature_req(ChVM *vm, ChValue req) {
    if (ch_is_symbol(req)) {
        return ch_feature_present(ch_as_symbol(req)->name);
    }
    if (!ch_is_pair(req) || !ch_is_symbol(ch_car(req))) {
        return 0;
    }
    const char *op = ch_symbol_basename(ch_as_symbol(ch_car(req)));
    ChValue rest = ch_cdr(req);
    if (strcmp(op, "and") == 0) {
        for (ChValue p = rest; ch_is_pair(p); p = ch_cdr(p)) {
            if (!ch_eval_feature_req(vm, ch_car(p))) {
                return 0;
            }
        }
        return 1;
    }
    if (strcmp(op, "or") == 0) {
        for (ChValue p = rest; ch_is_pair(p); p = ch_cdr(p)) {
            if (ch_eval_feature_req(vm, ch_car(p))) {
                return 1;
            }
        }
        return 0;
    }
    if (strcmp(op, "not") == 0) {
        if (!ch_is_pair(rest)) {
            return 0;
        }
        return !ch_eval_feature_req(vm, ch_car(rest));
    }
    if (strcmp(op, "library") == 0) {
        if (!ch_is_pair(rest)) {
            return 0;
        }
        return library_available(vm, ch_car(rest));
    }
    return 0;
}

ChValue ch_features_list(ChVM *vm) {
    ChValue list = CH_NIL;
    ch_gc_push(&vm->gc, &list);
    for (size_t i = sizeof(k_features) / sizeof(k_features[0]); i > 0; i--) {
        ChValue sym = ch_gc_intern_symbol_cstr(&vm->gc, k_features[i - 1]);
        list = ch_gc_cons(&vm->gc, sym, list);
    }
    ch_gc_pop(&vm->gc);
    return list;
}

static void define_prim(ChVM *vm, const char *name, ChNativeFn fn, int arity, int min_arity) {
    ChValue sym = ch_gc_intern_symbol_cstr(&vm->gc, name);
    int idx = ch_vm_intern_global(vm, ch_as_symbol(sym));
    ChValue nv = ch_gc_make_native(&vm->gc, fn, name, arity, min_arity);
    ch_vm_define_global(vm, idx, nv);
}

static ChValue prim_features(ChVM *vm, ChValue *args, int nargs) {
    (void)args;
    if (nargs != 0) {
        snprintf(vm->error, sizeof(vm->error), "features: expected 0 arguments");
        return CH_UNDEFINED;
    }
    return ch_features_list(vm);
}

void ch_register_features_primitives(ChVM *vm) {
    define_prim(vm, "features", prim_features, 0, 0);
}
