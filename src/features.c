#include "chaaya/features.h"

#include "chaaya/library.h"
#include "chaaya/opcode.h"
#include "chaaya/version.h"
#include "chaaya/vm.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static const char *const k_features[] = {
    "r7rs",
    "chaaya",
    /* Kaappi-compatible cond-expand id for shared deferred/portable suites. */
    "kaappi",
    "chaaya-fibers",
#if !defined(__wasi__)
    "chaaya-ffi",
#endif
    "chaaya-reactor",
    "kaappi-reactor",
#if !defined(__wasi__)
    "chaaya-threads",
    "kaappi-threads",
#endif
    "ieee-float",
    "exact-closed",
#if defined(__wasi__)
    "wasm32",
    "wasi",
#elif defined(__APPLE__)
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

static const int k_builtin_srfis[] = {
    1, 9, 13, 18, 39, 69, 133, 170, 192, 254, 258, 260,
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

static int parse_srfi_filename(const char *name, int *out_num) {
    size_t len = strlen(name);
    if (len < 5 || strcmp(name + len - 4, ".sld") != 0) {
        return 0;
    }
    const char *digits = name;
    for (const char *p = digits; p < name + len - 4; p++) {
        if (!isdigit((unsigned char)*p)) {
            return 0;
        }
    }
    errno = 0;
    char *end = NULL;
    long long parsed = strtoll(digits, &end, 10);
    if (errno != 0 || end != name + len - 4 || parsed < 0 || parsed > 9999) {
        return 0;
    }
    *out_num = (int)parsed;
    return 1;
}

static int srfi_list_contains(int *nums, size_t count, int n) {
    for (size_t i = 0; i < count; i++) {
        if (nums[i] == n) {
            return 1;
        }
    }
    return 0;
}

static size_t collect_portable_srfis(const char **lib_paths, size_t lib_path_count, int *out,
                                     size_t max_out) {
    static const char *default_paths[] = {"./lib"};
    if (lib_path_count == 0) {
        lib_paths = default_paths;
        lib_path_count = 1;
    }
    size_t count = 0;
    for (size_t p = 0; p < lib_path_count; p++) {
        char dir[512];
        if (snprintf(dir, sizeof(dir), "%s/srfi", lib_paths[p]) >= (int)sizeof(dir)) {
            continue;
        }
        DIR *d = opendir(dir);
        if (!d) {
            continue;
        }
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
            int num = 0;
            if (!parse_srfi_filename(ent->d_name, &num)) {
                continue;
            }
            if (srfi_list_contains(out, count, num)) {
                continue;
            }
            if (count < max_out) {
                out[count++] = num;
            }
        }
        closedir(d);
    }
    for (size_t i = 0; i + 1 < count; i++) {
        for (size_t j = i + 1; j < count; j++) {
            if (out[j] < out[i]) {
                int tmp = out[i];
                out[i] = out[j];
                out[j] = tmp;
            }
        }
    }
    return count;
}

static size_t count_defined_primitives(void) {
    ChVM vm;
    ch_vm_init(&vm);
    ch_vm_register_primitives(&vm);
    size_t n = 0;
    for (size_t i = 0; i < vm.global_count; i++) {
        if (vm.globals[i].defined) {
            n++;
        }
    }
    ch_vm_deinit(&vm);
    return n;
}

static void json_escape_string(FILE *out, const char *s) {
    fputc('"', out);
    for (const char *p = s; *p; p++) {
        if (*p == '"' || *p == '\\') {
            fputc('\\', out);
        }
        fputc(*p, out);
    }
    fputc('"', out);
}

static void print_number_list(FILE *out, const int *nums, size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (i > 0) {
            fputc(' ', out);
        }
        fprintf(out, "%d", nums[i]);
    }
}

static void print_json_number_array(FILE *out, const int *nums, size_t count) {
    fputc('[', out);
    for (size_t i = 0; i < count; i++) {
        if (i > 0) {
            fputc(',', out);
        }
        fprintf(out, "%d", nums[i]);
    }
    fputc(']', out);
}

void ch_features_print_text(FILE *out) {
    int portable[256];
    size_t portable_count = collect_portable_srfis(NULL, 0, portable, sizeof(portable) / sizeof(portable[0]));

    fprintf(out, "%s\n", CHAAYA_VERSION_BANNER);
    fprintf(out, "language:     C23\n");
#ifdef CHAAYA_HAS_LINENOISE
    fprintf(out, "linenoise:    yes\n");
#else
    fprintf(out, "linenoise:    no\n");
#endif
    fprintf(out, "primitives:   %zu\n", count_defined_primitives());
    fprintf(out, "sandbox:      available\n");
    fprintf(out, "stage:        bootstrap\n");
    fprintf(out, "opcodes:      %d\n", (int)CH_OP_HALT + 1);
    fprintf(out, "call/cc:      yes\n");
    fprintf(out, "dynamic-wind: yes\n");
    fprintf(out, "exceptions:   yes\n");
    fprintf(out, "libraries:    yes\n");
    fprintf(out, "macros:       yes\n");
    fprintf(out, "native:       mvp (--native / compile; partial lowering)\n");
    fprintf(out, "wasm:         mvp (chaaya wasm → build-wasm.sh)\n");
    fprintf(out, "lsp:          mvp (diagnostics/symbols/completion/hover/def/refs)\n");

    fputs("\nFeatures (cond-expand identifiers):\n  ", out);
    for (size_t i = 0; i < sizeof(k_features) / sizeof(k_features[0]); i++) {
        if (i > 0) {
            fputc(' ', out);
        }
        fputs(k_features[i], out);
    }
    fputc('\n', out);

    fprintf(out, "\nSRFIs:\n  built-in (%zu): ", sizeof(k_builtin_srfis) / sizeof(k_builtin_srfis[0]));
    print_number_list(out, k_builtin_srfis, sizeof(k_builtin_srfis) / sizeof(k_builtin_srfis[0]));
    fprintf(out, "\n  portable (%zu): ", portable_count);
    print_number_list(out, portable, portable_count);
    fputs("\n  (each is also a cond-expand feature id: srfi-<n>)\n", out);

    fprintf(out, "\nLimits:\n");
    fprintf(out, "  initial frame capacity     %d\n", CH_VM_MAX_FRAMES);
    fprintf(out, "  initial register capacity  %d\n", CH_VM_MAX_REGS);
    fprintf(out, "  gc initial threshold       %d\n", CH_GC_DEFAULT_THRESHOLD);
}

int ch_features_print_json(FILE *out, const char **lib_paths, size_t lib_path_count) {
    int portable[256];
    size_t portable_count =
        collect_portable_srfis(lib_paths, lib_path_count, portable, sizeof(portable) / sizeof(portable[0]));

    fputc('{', out);
    fputs("\"implementation\":", out);
    json_escape_string(out, "chaaya");
    fputs(",\"version\":", out);
    json_escape_string(out, CHAAYA_VERSION);
    fputs(",\"language\":", out);
    json_escape_string(out, "C23");
#ifdef CHAAYA_HAS_LINENOISE
    fputs(",\"linenoise\":true", out);
#else
    fputs(",\"linenoise\":false", out);
#endif
    fprintf(out, ",\"primitives\":%zu", count_defined_primitives());
    fputs(",\"sandbox_available\":true", out);
    fputs(",\"stage\":", out);
    json_escape_string(out, "bootstrap");
    fprintf(out, ",\"opcodes\":%d", (int)CH_OP_HALT + 1);
    fputs(",\"call_cc\":true,\"dynamic_wind\":true,\"exceptions\":true,\"libraries\":true,\"macros\":"
          "true",
          out);
    fputs(",\"native_backend\":", out);
    json_escape_string(out, "mvp");
    fputs(",\"wasm_backend\":", out);
    json_escape_string(out, "mvp");
    fputs(",\"lsp\":", out);
    json_escape_string(out, "mvp");
    fputs(",\"features\":[", out);
    for (size_t i = 0; i < sizeof(k_features) / sizeof(k_features[0]); i++) {
        if (i > 0) {
            fputc(',', out);
        }
        json_escape_string(out, k_features[i]);
    }
    fputc(']', out);
    fputs(",\"srfis\":{\"builtin\":", out);
    print_json_number_array(out, k_builtin_srfis, sizeof(k_builtin_srfis) / sizeof(k_builtin_srfis[0]));
    fputs(",\"portable\":", out);
    print_json_number_array(out, portable, portable_count);
    fputs("}", out);
    fprintf(out,
            ",\"limits\":{\"initial_frame_capacity\":%d,\"initial_register_capacity\":%d,"
            "\"gc_initial_threshold\":%d}",
            CH_VM_MAX_FRAMES, CH_VM_MAX_REGS, CH_GC_DEFAULT_THRESHOLD);
    fputs("}\n", out);
    return 0;
}
