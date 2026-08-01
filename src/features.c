#include "chaaya/features.h"

#include "chaaya/library.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const k_features[] = {
    "r7rs",
    "chaaya",
    "chaaya-fibers",
    "chaaya-ffi",
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

static int parse_srfi_feature_number(const char *name, int64_t *out);
static int parse_srfi261_suffix_number(const char *name, int64_t *out);

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
    if (ok) {
        return 1;
    }

    /* SRFI 261 fallback: (srfi <mnemonic>-<n>) -> (srfi <n>) when direct form is absent. */
    if (ch_is_pair(name_list) && ch_is_symbol(ch_car(name_list)) &&
        strcmp(ch_symbol_basename(ch_as_symbol(ch_car(name_list))), "srfi") == 0) {
        ChValue rest = ch_cdr(name_list);
        if (ch_is_pair(rest) && ch_is_symbol(ch_car(rest))) {
            int64_t n = 0;
            if (parse_srfi261_suffix_number(ch_as_symbol(ch_car(rest))->name, &n)) {
                if (n == 261) {
                    return 1;
                }
                char fallback_rel[64];
                if (snprintf(fallback_rel, sizeof(fallback_rel), "srfi/%lld.sld", (long long)n) <
                    (int)sizeof(fallback_rel)) {
                    return ch_library_file_exists(vm, fallback_rel);
                }
            }
        }
    }
    return 0;
}

static int parse_srfi_feature_number(const char *name, int64_t *out) {
    static const char prefix[] = "srfi-";
    const size_t prefix_len = sizeof(prefix) - 1;
    if (strncmp(name, prefix, prefix_len) != 0) {
        return 0;
    }

    const char *digits = name + prefix_len;
    if (*digits == '\0') {
        return 0;
    }
    for (const char *p = digits; *p; p++) {
        if (!isdigit((unsigned char)*p)) {
            return 0;
        }
    }

    errno = 0;
    char *end = NULL;
    long long parsed = strtoll(digits, &end, 10);
    if (errno != 0 || !end || *end != '\0' || parsed < 0 || parsed > CH_FIXNUM_MAX) {
        return 0;
    }
    *out = (int64_t)parsed;
    return 1;
}

static int parse_srfi261_suffix_number(const char *name, int64_t *out) {
    const char *dash = strrchr(name, '-');
    if (!dash || dash == name || dash[1] == '\0') {
        return 0;
    }
    for (const char *p = dash + 1; *p; p++) {
        if (!isdigit((unsigned char)*p)) {
            return 0;
        }
    }
    errno = 0;
    char *end = NULL;
    long long parsed = strtoll(dash + 1, &end, 10);
    if (errno != 0 || !end || *end != '\0' || parsed < 0 || parsed > CH_FIXNUM_MAX) {
        return 0;
    }
    *out = (int64_t)parsed;
    return 1;
}

static int srfi_feature_available(ChVM *vm, const char *name) {
    int64_t srfi_num = 0;
    if (!parse_srfi_feature_number(name, &srfi_num)) {
        return 0;
    }
    if (srfi_num == 261) {
        return 1; /* SRFI 261 is a naming convention (no .sld required). */
    }

    char dotted[64];
    if (snprintf(dotted, sizeof(dotted), "srfi.%lld", (long long)srfi_num) >= (int)sizeof(dotted)) {
        return 0;
    }
    if (vm->libraries && ch_library_lookup(vm->libraries, dotted)) {
        return 1;
    }

    char rel[64];
    if (snprintf(rel, sizeof(rel), "srfi/%lld.sld", (long long)srfi_num) >= (int)sizeof(rel)) {
        return 0;
    }
    return ch_library_file_exists(vm, rel);
}

int ch_eval_feature_req(ChVM *vm, ChValue req) {
    if (ch_is_symbol(req)) {
        const char *name = ch_as_symbol(req)->name;
        if (ch_feature_present(name)) {
            return 1;
        }
        return srfi_feature_available(vm, name);
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
