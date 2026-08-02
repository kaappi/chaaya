#include "chaaya/library.h"

#include "chaaya/compiler.h"
#include "chaaya/coverage.h"
#include "chaaya/eval.h"
#include "chaaya/expander.h"
#include "chaaya/features.h"
#include "chaaya/reader.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

void ch_library_registry_init(ChLibraryRegistry *reg) {
    memset(reg, 0, sizeof(*reg));
}

void ch_library_registry_deinit(ChLibraryRegistry *reg) {
    for (size_t i = 0; i < reg->count; i++) {
        free(reg->libs[i]->name);
        free(reg->libs[i]->runtime_env);
        free(reg->libs[i]);
    }
    memset(reg, 0, sizeof(*reg));
}

ChLibrary *ch_library_lookup(ChLibraryRegistry *reg, const char *name) {
    for (size_t i = 0; i < reg->count; i++) {
        if (strcmp(reg->libs[i]->name, name) == 0) {
            return reg->libs[i];
        }
    }
    return NULL;
}

int ch_library_register(ChLibraryRegistry *reg, ChLibrary *lib) {
    ChLibrary *existing = ch_library_lookup(reg, lib->name);
    if (existing) {
        /* Replace in place */
        for (size_t i = 0; i < reg->count; i++) {
            if (reg->libs[i] == existing) {
                free(existing->name);
                free(existing->runtime_env);
                free(existing);
                reg->libs[i] = lib;
                break;
            }
        }
    } else {
        if (reg->count >= CH_LIB_MAX_LIBS) {
            return -1;
        }
        reg->libs[reg->count++] = lib;
    }
    if (ch_coverage_enabled() && lib) {
        for (size_t i = 0; i < lib->export_count; i++) {
            if (lib->export_names[i]) {
                ch_coverage_register(lib->name, lib->export_names[i]->name);
            }
        }
    }
    return 0;
}

char *ch_library_name_to_string(ChValue name_list) {
    char buf[512];
    size_t pos = 0;
    buf[0] = '\0';
    for (ChValue p = name_list; ch_is_pair(p); p = ch_cdr(p)) {
        ChValue part = ch_car(p);
        const char *s;
        char num[32];
        if (ch_is_symbol(part)) {
            s = ch_as_symbol(part)->name;
        } else if (ch_is_fixnum(part)) {
            snprintf(num, sizeof(num), "%lld", (long long)ch_to_fixnum(part));
            s = num;
        } else {
            return NULL;
        }
        size_t sl = strlen(s);
        if (pos + sl + 2 >= sizeof(buf)) {
            return NULL;
        }
        if (pos > 0) {
            buf[pos++] = '.';
        }
        memcpy(buf + pos, s, sl);
        pos += sl;
        buf[pos] = '\0';
    }
    return strdup(buf);
}

char *ch_library_name_to_path(ChValue name_list) {
    char buf[512];
    size_t pos = 0;
    buf[0] = '\0';
    for (ChValue p = name_list; ch_is_pair(p); p = ch_cdr(p)) {
        ChValue part = ch_car(p);
        const char *s;
        char num[32];
        if (ch_is_symbol(part)) {
            s = ch_as_symbol(part)->name;
        } else if (ch_is_fixnum(part)) {
            snprintf(num, sizeof(num), "%lld", (long long)ch_to_fixnum(part));
            s = num;
        } else {
            return NULL;
        }
        size_t sl = strlen(s);
        if (pos + sl + 6 >= sizeof(buf)) {
            return NULL;
        }
        if (pos > 0) {
            buf[pos++] = '/';
        }
        memcpy(buf + pos, s, sl);
        pos += sl;
        buf[pos] = '\0';
    }
    if (pos + 5 >= sizeof(buf)) {
        return NULL;
    }
    memcpy(buf + pos, ".sld", 5);
    return strdup(buf);
}

int ch_lib_env_find(const ChLibEnv *env, ChSymbol *sym) {
    for (size_t i = 0; i < env->count; i++) {
        if (env->bindings[i].name == sym) {
            return (int)i;
        }
    }
    return -1;
}

int ch_lib_env_intern(ChLibEnv *env, ChSymbol *sym) {
    int idx = ch_lib_env_find(env, sym);
    if (idx >= 0) {
        return idx;
    }
    if (env->count >= CH_LIB_ENV_MAX) {
        return -1;
    }
    idx = (int)env->count++;
    env->bindings[idx].name = sym;
    env->bindings[idx].value = CH_UNDEFINED;
    env->bindings[idx].defined = false;
    return idx;
}

void ch_lib_env_define(ChLibEnv *env, int idx, ChValue v) {
    env->bindings[idx].value = v;
    env->bindings[idx].defined = true;
}

static int register_exports_from_globals(ChVM *vm, const char *lib_name, const char *const *names,
                                         size_t nnames) {
    ChLibrary *lib = (ChLibrary *)calloc(1, sizeof(ChLibrary));
    if (!lib) {
        return -1;
    }
    lib->name = strdup(lib_name);
    if (!lib->name) {
        free(lib);
        return -1;
    }
    for (size_t i = 0; i < nnames; i++) {
        ChValue symv = ch_gc_intern_symbol_cstr(&vm->gc, names[i]);
        ChSymbol *sym = ch_as_symbol(symv);
        int g = -1;
        for (size_t j = 0; j < vm->global_count; j++) {
            if (vm->globals[j].name == sym && vm->globals[j].defined) {
                g = (int)j;
                break;
            }
        }
        if (g < 0) {
            continue; /* skip missing; keeps subsets soft while surface grows */
        }
        if (lib->export_count >= CH_LIB_MAX_EXPORTS) {
            free(lib->name);
            free(lib);
            return -1;
        }
        lib->export_names[lib->export_count] = sym;
        lib->export_values[lib->export_count] = vm->globals[g].value;
        lib->export_count++;
    }
    return ch_library_register(vm->libraries, lib);
}

int ch_register_scheme_base_library(ChVM *vm) {
    ChLibrary *lib = (ChLibrary *)calloc(1, sizeof(ChLibrary));
    if (!lib) {
        return -1;
    }
    lib->name = strdup("scheme.base");
    for (size_t i = 0; i < vm->global_count && lib->export_count < CH_LIB_MAX_EXPORTS; i++) {
        if (!vm->globals[i].defined) {
            continue;
        }
        const char *n = vm->globals[i].name->name;
        if (n[0] == '%') {
            continue; /* internals */
        }
        lib->export_names[lib->export_count] = vm->globals[i].name;
        lib->export_values[lib->export_count] = vm->globals[i].value;
        lib->export_count++;
    }
    return ch_library_register(vm->libraries, lib);
}

static int register_scheme_r5rs_library(ChVM *vm) {
    /* R5RS report environment: same surface as (scheme base) for Chaaya MVP. */
    ChLibrary *base = ch_library_lookup(vm->libraries, "scheme.base");
    if (!base) {
        return -1;
    }
    ChLibrary *lib = (ChLibrary *)calloc(1, sizeof(ChLibrary));
    if (!lib) {
        return -1;
    }
    lib->name = strdup("scheme.r5rs");
    if (!lib->name) {
        free(lib);
        return -1;
    }
    for (size_t i = 0; i < base->export_count && lib->export_count < CH_LIB_MAX_EXPORTS; i++) {
        lib->export_names[lib->export_count] = base->export_names[i];
        lib->export_values[lib->export_count] = base->export_values[i];
        lib->export_count++;
    }
    return ch_library_register(vm->libraries, lib);
}

int ch_register_builtin_libraries(ChVM *vm) {
    if (ch_register_scheme_base_library(vm) != 0) {
        return -1;
    }
    static const char *const write_exports[] = {"display", "write", "write-shared", "write-simple",
                                                "newline", "write-u8", "write-bytevector"};
    static const char *const read_exports[] = {"read",           "read-char",      "peek-char",
                                               "read-u8",        "peek-u8",         "read-bytevector",
                                               "read-bytevector!", "u8-ready?",     "eof-object",
                                               "eof-object?"};
    static const char *const cxr_exports[] = {"caar", "cadr", "cdar", "cddr"};
    static const char *const char_exports[] = {
        "char?",           "char=?",          "char<?",          "char<=?",
        "char>?",          "char>=?",         "char-ci=?",
        "char-ci<?",       "char-ci<=?",      "char-ci>?",       "char-ci>=?",
        "char-alphabetic?", "char-numeric?",  "char-whitespace?",
        "char-upper-case?", "char-lower-case?",
        "char-upcase",     "char-downcase",  "char-foldcase", "digit-value",
        "char->integer",   "integer->char",
        "string-ci=?",     "string-ci<?",     "string-ci<=?",    "string-ci>?",
        "string-ci>=?",    "string-upcase",   "string-downcase", "string-foldcase",
    };
    static const char *const process_exports[] = {
        "exit", "emergency-exit", "command-line", "features",
        "get-environment-variable", "get-environment-variables"};
    static const char *const lazy_exports[] = {"delay", "force", "promise?", "make-promise"};
    static const char *const file_exports[] = {
        "call-with-input-file", "call-with-output-file", "with-input-from-file",
        "with-output-to-file",  "open-input-file",       "open-output-file",
        "open-binary-input-file", "open-binary-output-file",
        "file-exists?",         "delete-file"};
    static const char *const complex_exports[] = {
        "angle", "imag-part", "magnitude", "make-polar", "make-rectangular", "real-part"};
    static const char *const inexact_exports[] = {
        "acos", "asin", "atan", "cos", "exp", "finite?", "infinite?", "log", "nan?", "sin", "sqrt",
        "tan"};
    static const char *const exact_exports[] = {"exact", "exact-integer-sqrt", "inexact"};
    static const char *const eval_exports[] = {
        "eval", "environment", "null-environment", "scheme-report-environment"};
    static const char *const load_exports[] = {"load"};
    static const char *const repl_exports[] = {"interaction-environment"};
    static const char *const time_exports[] = {"current-second", "current-jiffy", "jiffies-per-second",
                                               "time?", "make-time", "time-type", "time-second",
                                               "time-nanosecond"};
    static const char *const chaaya_fibers_exports[] = {
        "spawn-fiber", "spawn", "fiber-yield", "yield", "fiber?", "fiber-join",
        "make-channel", "channel?", "channel-send!", "channel-send", "channel-recv",
        "channel-receive", "channel-get", "channel-close!", "channel-closed?",
        "channel-timeout-exception?"};
    static const char *const chaaya_ffi_exports[] = {"open-foreign-library",
                                                      "close-foreign-library!",
                                                      "foreign-library?",
                                                      "foreign-procedure",
                                                      "foreign-procedure?",
                                                      "ffi-open",
                                                      "ffi-close",
                                                      "ffi-fn",
                                                      "ffi-callback",
                                                      "ffi-callback-release",
                                                      "ffi-callback?"};
    static const char *const chaaya_primitives_exports[] = {
        "%default-random-source", "%rs-next-int", "%rs-next-real"};
    static const char *const srfi170_exports[] = {
        "directory-files",         "file-info",           "file-info?",
        "file-info-directory?",    "file-info-regular?",  "file-info-symlink?",
        "file-info:size",          "file-info:mtime",     "file-info:mode",
        "file-info-type",          "temp-file-prefix",    "create-temp-file",
        "create-directory",        "delete-directory",    "rename-file",
        "real-path",               "current-directory",   "set-current-directory!",
        "file-exists?",            "delete-file"};
    static const char *const srfi18_exports[] = {
        "make-thread",      "thread-start!",    "thread-join!",     "thread-sleep!",
        "thread-yield!",    "current-thread",   "thread?",          "thread-name",
        "make-mutex",       "mutex?",           "mutex-lock!",      "mutex-unlock!",
        "make-condition-variable", "condition-variable?",
        "condition-variable-signal!", "condition-variable-broadcast!"};
    static const char *const srfi254_exports[] = {
        "make-ephemeron",   "ephemeron?",       "ephemeron-key",    "ephemeron-value",
        "ephemeron-broken?", "ephemeron-ref",   "reference-barrier"};
    static const char *const srfi1_exports[] = {
        "fold", "fold-right", "reduce", "reduce-right", "filter", "remove", "partition",
        "find", "find-tail", "any", "every", "count", "iota", "zip", "concatenate",
        "take", "drop", "take-while", "drop-while", "filter-map", "append-map", "last",
        "last-pair", "proper-list?", "dotted-list?", "circular-list?", "lset-intersection",
        "lset-difference", "lset=", "lset-adjoin", "lset-union", "lset-xor", "xcons",
        "cons*", "list-tabulate", "circular-list", "not-pair?", "null-list?", "list=",
        "first", "second", "third", "fourth", "fifth", "sixth", "seventh", "eighth",
        "ninth", "tenth", "car+cdr", "take-right", "drop-right", "split-at", "list-index",
        "span", "break", "delete", "delete-duplicates", "alist-cons", "alist-copy",
        "alist-delete", "unfold", "unfold-right", "append-reverse", "length+", "unzip1",
        "unzip2", "pair-for-each", "pair-fold", "pair-fold-right", "map-in-order",
        "map", "for-each", "length", "append", "reverse", "list-ref", "list-tail",
        "list-set!", "list-copy", "make-list", "memq", "memv", "member", "assoc", "assq",
        "assv",
    };
    static const char *const srfi13_exports[] = {
        "string-null?", "string-concatenate", "string-prefix?", "string-suffix?",
        "string-contains", "string-unfold", "string-unfold-right", "string-index-right",
        "string-skip", "string-skip-right", "string-index", "string-take", "string-drop",
        "string-trim", "string-trim-right", "string-trim-both",
        "string-length", "string-append", "string-ref", "string-set!", "string-copy",
        "string=?", "string<?", "string<=?", "string>?", "string>=?", "substring",
    };
    static const char *const srfi39_exports[] = {"make-parameter"};
    static const char *const srfi69_exports[] = {
        "make-hash-table", "hash-table?", "hash-table-ref", "hash-table-set!",
        "hash-table-delete!", "hash-table-size", "hash-table-keys", "hash-table-values",
        "hash-table-walk", "hash-table-fold", "hash-table-exists?", "hash-table-ref/default",
        "hash-table-update!", "hash-table-update!/default", "hash-table->alist",
        "alist->hash-table", "hash-table-merge!", "hash", "string-hash", "string-ci-hash",
        "hash-by-identity",
    };
    static const char *const srfi133_exports[] = {
        "vector-empty?", "vector-count", "vector-any", "vector-every", "vector-index",
        "vector-index-right", "vector-skip", "vector-skip-right", "vector-swap!",
        "vector-reverse!", "vector-reverse-copy", "vector-unfold", "vector-unfold-right",
        "vector-fold", "vector-fold-right", "vector-map!", "vector-partition",
        "vector-concatenate", "vector=", "vector-length", "vector-ref", "vector-set!",
        "vector-copy", "vector-append", "vector-map", "vector-for-each", "make-vector",
        "vector->list", "list->vector",
    };
    static const char *const srfi192_exports[] = {
        "port-position", "set-port-position!", "port-has-port-position?",
        "port-has-set-port-position!?",
    };
    static const char *const srfi258_exports[] = {
        "string->uninterned-symbol", "symbol-interned?", "generate-uninterned-symbol",
    };
    static const char *const srfi260_exports[] = {"generate-symbol"};
    if (register_exports_from_globals(vm, "scheme.write", write_exports,
                                      sizeof(write_exports) / sizeof(write_exports[0])) != 0) {
        return -1;
    }
    if (register_exports_from_globals(vm, "scheme.read", read_exports,
                                      sizeof(read_exports) / sizeof(read_exports[0])) != 0) {
        return -1;
    }
    if (register_exports_from_globals(vm, "scheme.cxr", cxr_exports,
                                      sizeof(cxr_exports) / sizeof(cxr_exports[0])) != 0) {
        return -1;
    }
    if (register_exports_from_globals(vm, "scheme.char", char_exports,
                                      sizeof(char_exports) / sizeof(char_exports[0])) != 0) {
        return -1;
    }
    if (register_exports_from_globals(vm, "scheme.process-context", process_exports,
                                      sizeof(process_exports) / sizeof(process_exports[0])) != 0) {
        return -1;
    }
    if (register_exports_from_globals(vm, "scheme.lazy", lazy_exports,
                                      sizeof(lazy_exports) / sizeof(lazy_exports[0])) != 0) {
        return -1;
    }
    if (register_exports_from_globals(vm, "scheme.file", file_exports,
                                      sizeof(file_exports) / sizeof(file_exports[0])) != 0) {
        return -1;
    }
    if (register_exports_from_globals(vm, "scheme.complex", complex_exports,
                                      sizeof(complex_exports) / sizeof(complex_exports[0])) != 0) {
        return -1;
    }
    if (register_exports_from_globals(vm, "scheme.inexact", inexact_exports,
                                      sizeof(inexact_exports) / sizeof(inexact_exports[0])) != 0) {
        return -1;
    }
    if (register_exports_from_globals(vm, "scheme.exact", exact_exports,
                                      sizeof(exact_exports) / sizeof(exact_exports[0])) != 0) {
        return -1;
    }
    if (register_exports_from_globals(vm, "scheme.eval", eval_exports,
                                      sizeof(eval_exports) / sizeof(eval_exports[0])) != 0) {
        return -1;
    }
    if (register_exports_from_globals(vm, "scheme.load", load_exports,
                                      sizeof(load_exports) / sizeof(load_exports[0])) != 0) {
        return -1;
    }
    if (register_exports_from_globals(vm, "scheme.repl", repl_exports,
                                      sizeof(repl_exports) / sizeof(repl_exports[0])) != 0) {
        return -1;
    }
    if (register_exports_from_globals(vm, "scheme.time", time_exports,
                                      sizeof(time_exports) / sizeof(time_exports[0])) != 0) {
        return -1;
    }
    if (register_exports_from_globals(vm, "chaaya.fibers", chaaya_fibers_exports,
                                      sizeof(chaaya_fibers_exports) /
                                          sizeof(chaaya_fibers_exports[0])) != 0) {
        return -1;
    }
    if (register_exports_from_globals(vm, "kaappi.fibers", chaaya_fibers_exports,
                                      sizeof(chaaya_fibers_exports) /
                                          sizeof(chaaya_fibers_exports[0])) != 0) {
        return -1;
    }
    if (register_exports_from_globals(vm, "chaaya.ffi", chaaya_ffi_exports,
                                      sizeof(chaaya_ffi_exports) / sizeof(chaaya_ffi_exports[0])) !=
        0) {
        return -1;
    }
    if (register_exports_from_globals(vm, "kaappi.ffi", chaaya_ffi_exports,
                                      sizeof(chaaya_ffi_exports) / sizeof(chaaya_ffi_exports[0])) !=
        0) {
        return -1;
    }
    if (register_exports_from_globals(vm, "chaaya.primitives", chaaya_primitives_exports,
                                      sizeof(chaaya_primitives_exports) /
                                          sizeof(chaaya_primitives_exports[0])) != 0) {
        return -1;
    }
    if (register_exports_from_globals(vm, "srfi.170", srfi170_exports,
                                      sizeof(srfi170_exports) / sizeof(srfi170_exports[0])) != 0) {
        return -1;
    }
    if (register_exports_from_globals(vm, "srfi.18", srfi18_exports,
                                      sizeof(srfi18_exports) / sizeof(srfi18_exports[0])) != 0) {
        return -1;
    }
    if (register_exports_from_globals(vm, "srfi.254", srfi254_exports,
                                      sizeof(srfi254_exports) / sizeof(srfi254_exports[0])) != 0) {
        return -1;
    }
    if (register_exports_from_globals(vm, "srfi.1", srfi1_exports,
                                      sizeof(srfi1_exports) / sizeof(srfi1_exports[0])) != 0) {
        return -1;
    }
    if (register_exports_from_globals(vm, "srfi.13", srfi13_exports,
                                      sizeof(srfi13_exports) / sizeof(srfi13_exports[0])) != 0) {
        return -1;
    }
    if (register_exports_from_globals(vm, "srfi.39", srfi39_exports,
                                      sizeof(srfi39_exports) / sizeof(srfi39_exports[0])) != 0) {
        return -1;
    }
    if (register_exports_from_globals(vm, "srfi.69", srfi69_exports,
                                      sizeof(srfi69_exports) / sizeof(srfi69_exports[0])) != 0) {
        return -1;
    }
    if (register_exports_from_globals(vm, "srfi.133", srfi133_exports,
                                      sizeof(srfi133_exports) / sizeof(srfi133_exports[0])) != 0) {
        return -1;
    }
    if (register_exports_from_globals(vm, "srfi.192", srfi192_exports,
                                      sizeof(srfi192_exports) / sizeof(srfi192_exports[0])) != 0) {
        return -1;
    }
    if (register_exports_from_globals(vm, "srfi.258", srfi258_exports,
                                      sizeof(srfi258_exports) / sizeof(srfi258_exports[0])) != 0) {
        return -1;
    }
    if (register_exports_from_globals(vm, "srfi.260", srfi260_exports,
                                      sizeof(srfi260_exports) / sizeof(srfi260_exports[0])) != 0) {
        return -1;
    }
    /* SRFI 9 (define-record-type) is syntax-only; library exists for import. */
    if (register_exports_from_globals(vm, "srfi.9", NULL, 0) != 0) {
        return -1;
    }
    /* case-lambda is a compiler special form; library exists for R7RS import. */
    if (register_exports_from_globals(vm, "scheme.case-lambda", NULL, 0) != 0) {
        return -1;
    }
    if (register_scheme_r5rs_library(vm) != 0) {
        return -1;
    }
    return 0;
}

static int library_into_env(ChLibEnv *env, ChLibrary *lib) {
    for (size_t i = 0; i < lib->export_count; i++) {
        int idx = ch_lib_env_intern(env, lib->export_names[i]);
        if (idx < 0) {
            return -1;
        }
        ch_lib_env_define(env, idx, lib->export_values[i]);
    }
    return 0;
}

static int merge_env_into_globals(ChVM *vm, const ChLibEnv *env, ChLibEnv *macro_home) {
    for (size_t i = 0; i < env->count; i++) {
        if (!env->bindings[i].defined) {
            continue;
        }
        int idx = ch_vm_intern_global(vm, env->bindings[i].name);
        ch_vm_define_global(vm, idx, env->bindings[i].value);
        if (ch_is_transformer(env->bindings[i].value)) {
            ChLibEnv *saved_lib = vm->active_lib_env;
            if (macro_home) {
                vm->active_lib_env = macro_home;
            }
            int mrc = ch_vm_define_macro(vm, env->bindings[i].name,
                                         ch_as_transformer(env->bindings[i].value));
            vm->active_lib_env = saved_lib;
            if (mrc != 0) {
                snprintf(vm->error, sizeof(vm->error), "import: macro table full");
                return -1;
            }
        }
    }
    return 0;
}

static int merge_env_into_env(ChLibEnv *dst, const ChLibEnv *src) {
    for (size_t i = 0; i < src->count; i++) {
        if (!src->bindings[i].defined) {
            continue;
        }
        int idx = ch_lib_env_intern(dst, src->bindings[i].name);
        if (idx < 0) {
            return -1;
        }
        ch_lib_env_define(dst, idx, src->bindings[i].value);
    }
    return 0;
}

static int env_find_cstr(const ChLibEnv *env, const char *name) {
    for (size_t i = 0; i < env->count; i++) {
        if (env->bindings[i].defined && strcmp(env->bindings[i].name->name, name) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static int file_readable(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static char *dirname_dup(const char *path) {
    const char *slash = strrchr(path, '/');
    if (!slash) {
        return strdup(".");
    }
    size_t n = (size_t)(slash - path);
    if (n == 0) {
        return strdup("/");
    }
    char *d = (char *)malloc(n + 1);
    if (!d) {
        return NULL;
    }
    memcpy(d, path, n);
    d[n] = '\0';
    return d;
}

static char *join_path(const char *a, const char *b) {
    size_t la = strlen(a);
    size_t lb = strlen(b);
    int need_slash = (la > 0 && a[la - 1] != '/');
    char *out = (char *)malloc(la + lb + (need_slash ? 2 : 1));
    if (!out) {
        return NULL;
    }
    memcpy(out, a, la);
    size_t pos = la;
    if (need_slash) {
        out[pos++] = '/';
    }
    memcpy(out + pos, b, lb + 1);
    return out;
}

static char *resolve_library_path(ChVM *vm, const char *rel) {
    /* ./rel */
    if (file_readable(rel)) {
        return strdup(rel);
    }
    /* ./lib/rel */
    char *librel = join_path("lib", rel);
    if (librel && file_readable(librel)) {
        return librel;
    }
    free(librel);
    /* each --lib-path */
    for (size_t i = 0; i < vm->lib_path_count; i++) {
        char *p = join_path(vm->lib_paths[i], rel);
        if (p && file_readable(p)) {
            return p;
        }
        free(p);
    }
    /* script directory */
    if (vm->script_path) {
        char *dir = dirname_dup(vm->script_path);
        if (dir) {
            char *p = join_path(dir, rel);
            free(dir);
            if (p && file_readable(p)) {
                return p;
            }
            free(p);
        }
    }
    return NULL;
}

int ch_library_file_exists(ChVM *vm, const char *rel_path) {
    char *path = resolve_library_path(vm, rel_path);
    if (!path) {
        return 0;
    }
    free(path);
    return 1;
}

static char *resolve_include_path(ChVM *vm, const char *file) {
    if (!file || !file[0]) {
        return NULL;
    }
    if (file[0] == '/' && file_readable(file)) {
        return strdup(file);
    }
    if (vm->current_lib_dir) {
        char *p = join_path(vm->current_lib_dir, file);
        if (p && file_readable(p)) {
            return p;
        }
        free(p);
    }
    if (file_readable(file)) {
        return strdup(file);
    }
    if (vm->script_path) {
        char *dir = dirname_dup(vm->script_path);
        if (dir) {
            char *p = join_path(dir, file);
            free(dir);
            if (p && file_readable(p)) {
                return p;
            }
            free(p);
        }
    }
    return NULL;
}

static int eval_toplevel_form(ChVM *vm, ChValue expr);

size_t ch_library_push_gc_roots(ChVM *vm) {
    size_t n = 0;
    if (!vm->libraries) {
        return 0;
    }
    for (size_t i = 0; i < vm->libraries->count; i++) {
        ChLibrary *lib = vm->libraries->libs[i];
        for (size_t j = 0; j < lib->export_count; j++) {
            ch_gc_push(&vm->gc, &lib->export_values[j]);
            n++;
        }
        if (lib->runtime_env) {
            for (size_t j = 0; j < lib->runtime_env->count; j++) {
                if (lib->runtime_env->bindings[j].defined) {
                    ch_gc_push(&vm->gc, &lib->runtime_env->bindings[j].value);
                    n++;
                }
            }
        }
    }
    return n;
}

void ch_library_mark_gc_roots(ChVM *vm) {
    if (!vm->libraries) {
        return;
    }
    for (size_t i = 0; i < vm->libraries->count; i++) {
        ChLibrary *lib = vm->libraries->libs[i];
        for (size_t j = 0; j < lib->export_count; j++) {
            ch_gc_mark_value(lib->export_values[j]);
        }
        if (lib->runtime_env) {
            for (size_t j = 0; j < lib->runtime_env->count; j++) {
                if (lib->runtime_env->bindings[j].defined) {
                    ch_gc_mark_value(lib->runtime_env->bindings[j].value);
                }
            }
        }
    }
}

int ch_eval_toplevel_form(ChVM *vm, ChValue expr) {
    return eval_toplevel_form(vm, expr);
}

static int load_library_file(ChVM *vm, const char *path) {
    size_t len = 0;
    char *src = ch_read_file(path, &len);
    if (!src) {
        snprintf(vm->error, sizeof(vm->error), "cannot read library '%s'", path);
        return -1;
    }
    char *old_dir = vm->current_lib_dir;
    vm->current_lib_dir = dirname_dup(path);

    ChReader reader;
    ch_reader_init(&reader, &vm->gc, src, len);
    int rc = 0;
    for (;;) {
        ChValue expr = CH_NIL;
        ch_gc_push(&vm->gc, &expr);
        ChReadStatus rs = ch_read_datum(&reader, &expr);
        if (rs == CH_READ_EOF) {
            ch_gc_pop(&vm->gc);
            break;
        }
        if (rs != CH_READ_OK) {
            snprintf(vm->error, sizeof(vm->error), "read error in '%s': %s", path,
                     ch_reader_error(&reader));
            ch_gc_pop(&vm->gc);
            rc = -1;
            break;
        }
        if (eval_toplevel_form(vm, expr) != 0) {
            ch_gc_pop(&vm->gc);
            rc = -1;
            break;
        }
        ch_gc_pop(&vm->gc);
    }

    free(vm->current_lib_dir);
    vm->current_lib_dir = old_dir;
    free(src);
    return rc;
}

static int loading_push(ChVM *vm, const char *name) {
    for (size_t i = 0; i < vm->loading_lib_count; i++) {
        if (strcmp(vm->loading_libs[i], name) == 0) {
            snprintf(vm->error, sizeof(vm->error), "circular import of library (%s)", name);
            return -1;
        }
    }
    if (vm->loading_lib_count >= 32) {
        snprintf(vm->error, sizeof(vm->error), "import nesting too deep");
        return -1;
    }
    vm->loading_libs[vm->loading_lib_count++] = strdup(name);
    return 0;
}

static void loading_pop(ChVM *vm) {
    if (vm->loading_lib_count == 0) {
        return;
    }
    free(vm->loading_libs[--vm->loading_lib_count]);
    vm->loading_libs[vm->loading_lib_count] = NULL;
}

static int parse_srfi261_suffix_number(const char *name, int64_t *out_number) {
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
    *out_number = (int64_t)parsed;
    return 1;
}

static int srfi261_normalize_library_name(ChVM *vm, ChValue name_list, ChValue *out_name) {
    if (!ch_is_pair(name_list) || !ch_is_symbol(ch_car(name_list))) {
        return 0;
    }
    ChSymbol *head = ch_as_symbol(ch_car(name_list));
    if (strcmp(ch_symbol_basename(head), "srfi") != 0) {
        return 0;
    }

    ChValue tail = ch_cdr(name_list);
    if (!ch_is_pair(tail)) {
        return 0;
    }
    ChValue second = ch_car(tail);
    if (!ch_is_symbol(second)) {
        return 0;
    }

    int64_t number = 0;
    if (!parse_srfi261_suffix_number(ch_as_symbol(second)->name, &number)) {
        return 0;
    }

    ChValue normalized_tail = ch_cdr(tail);
    ChValue first = ch_car(name_list);
    ChValue normalized_name = CH_NIL;
    ch_gc_push(&vm->gc, &normalized_tail);
    ch_gc_push(&vm->gc, &first);
    ch_gc_push(&vm->gc, &normalized_name);
    normalized_name = ch_gc_cons(&vm->gc, ch_make_fixnum(number), normalized_tail);
    normalized_name = ch_gc_cons(&vm->gc, first, normalized_name);
    ch_gc_pop_n(&vm->gc, 3);
    *out_name = normalized_name;
    return 1;
}

ChLibrary *ch_ensure_library(ChVM *vm, ChValue name_list) {
    char *dotted = ch_library_name_to_string(name_list);
    if (!dotted) {
        snprintf(vm->error, sizeof(vm->error), "import: bad library name");
        return NULL;
    }
    ChLibrary *lib = ch_library_lookup(vm->libraries, dotted);
    if (lib) {
        free(dotted);
        return lib;
    }

    char *rel = ch_library_name_to_path(name_list);
    if (!rel) {
        snprintf(vm->error, sizeof(vm->error), "import: bad library name");
        free(dotted);
        return NULL;
    }
    char *path = resolve_library_path(vm, rel);
    free(rel);

    if (!path) {
        ChValue normalized_name = CH_NIL;
        if (srfi261_normalize_library_name(vm, name_list, &normalized_name)) {
            char *norm_dotted = ch_library_name_to_string(normalized_name);
            if (!norm_dotted) {
                snprintf(vm->error, sizeof(vm->error), "import: bad library name");
                free(dotted);
                return NULL;
            }

            lib = ch_library_lookup(vm->libraries, norm_dotted);
            if (lib) {
                free(norm_dotted);
                free(dotted);
                return lib;
            }

            char *norm_rel = ch_library_name_to_path(normalized_name);
            if (norm_rel) {
                char *norm_path = resolve_library_path(vm, norm_rel);
                free(norm_rel);
                if (norm_path) {
                    free(dotted);
                    dotted = norm_dotted;
                    norm_dotted = NULL;
                    path = norm_path;
                }
            }
            free(norm_dotted);
        }
    }

    if (!path) {
        snprintf(vm->error, sizeof(vm->error), "library not found: %s", dotted);
        free(dotted);
        return NULL;
    }

    if (loading_push(vm, dotted) != 0) {
        free(path);
        free(dotted);
        return NULL;
    }

    int rc = load_library_file(vm, path);
    free(path);
    loading_pop(vm);
    if (rc != 0) {
        free(dotted);
        return NULL;
    }
    lib = ch_library_lookup(vm->libraries, dotted);
    free(dotted);
    if (!lib) {
        snprintf(vm->error, sizeof(vm->error), "library file did not define the expected library");
        return NULL;
    }
    return lib;
}

static int resolve_import_set(ChVM *vm, ChValue set, ChLibEnv *out);

int ch_import_set_into_env(ChVM *vm, ChValue set, ChLibEnv *out) {
    return resolve_import_set(vm, set, out);
}

int ch_environment_from_imports(ChVM *vm, ChValue *import_sets, int nsets, ChLibEnv *out) {
    if (!out) {
        snprintf(vm->error, sizeof(vm->error), "environment: missing target environment");
        return -1;
    }
    for (int i = 0; i < nsets; i++) {
        if (ch_import_set_into_env(vm, import_sets[i], out) != 0) {
            return -1;
        }
    }
    return 0;
}

static int resolve_import_only(ChVM *vm, ChValue args, ChLibEnv *out) {
    /* (only <import-set> id ...) */
    if (!ch_is_pair(args)) {
        snprintf(vm->error, sizeof(vm->error), "import only: bad syntax");
        return -1;
    }
    ChLibEnv source;
    memset(&source, 0, sizeof(source));
    if (resolve_import_set(vm, ch_car(args), &source) != 0) {
        return -1;
    }
    for (ChValue ids = ch_cdr(args); ch_is_pair(ids); ids = ch_cdr(ids)) {
        ChValue id = ch_car(ids);
        if (!ch_is_symbol(id)) {
            snprintf(vm->error, sizeof(vm->error), "import only: expected identifier");
            return -1;
        }
        const char *name = ch_as_symbol(id)->name;
        int src = env_find_cstr(&source, name);
        if (src < 0) {
            snprintf(vm->error, sizeof(vm->error),
                     "import only: identifier '%s' not found in import set", name);
            return -1;
        }
        int idx = ch_lib_env_intern(out, ch_as_symbol(id));
        if (idx < 0) {
            snprintf(vm->error, sizeof(vm->error), "import only: environment full");
            return -1;
        }
        ch_lib_env_define(out, idx, source.bindings[src].value);
    }
    return 0;
}

static int resolve_import_except(ChVM *vm, ChValue args, ChLibEnv *out) {
    /* (except <import-set> id ...) */
    if (!ch_is_pair(args)) {
        snprintf(vm->error, sizeof(vm->error), "import except: bad syntax");
        return -1;
    }
    ChLibEnv source;
    memset(&source, 0, sizeof(source));
    if (resolve_import_set(vm, ch_car(args), &source) != 0) {
        return -1;
    }
    for (ChValue ids = ch_cdr(args); ch_is_pair(ids); ids = ch_cdr(ids)) {
        ChValue id = ch_car(ids);
        if (!ch_is_symbol(id)) {
            snprintf(vm->error, sizeof(vm->error), "import except: expected identifier");
            return -1;
        }
        const char *name = ch_as_symbol(id)->name;
        int src = env_find_cstr(&source, name);
        if (src < 0) {
            snprintf(vm->error, sizeof(vm->error),
                     "import except: identifier '%s' not found in import set", name);
            return -1;
        }
        source.bindings[src].defined = false;
    }
    return merge_env_into_env(out, &source);
}

static int resolve_import_prefix(ChVM *vm, ChValue args, ChLibEnv *out) {
    /* (prefix <import-set> prefix-id) */
    if (!ch_is_pair(args) || !ch_is_pair(ch_cdr(args))) {
        snprintf(vm->error, sizeof(vm->error), "import prefix: bad syntax");
        return -1;
    }
    ChValue pref = ch_car(ch_cdr(args));
    if (!ch_is_symbol(pref)) {
        snprintf(vm->error, sizeof(vm->error), "import prefix: expected identifier");
        return -1;
    }
    const char *prefix = ch_as_symbol(pref)->name;
    ChLibEnv source;
    memset(&source, 0, sizeof(source));
    if (resolve_import_set(vm, ch_car(args), &source) != 0) {
        return -1;
    }
    for (size_t i = 0; i < source.count; i++) {
        if (!source.bindings[i].defined) {
            continue;
        }
        char buf[256];
        if (snprintf(buf, sizeof(buf), "%s%s", prefix, source.bindings[i].name->name) >=
            (int)sizeof(buf)) {
            snprintf(vm->error, sizeof(vm->error), "import prefix: name too long");
            return -1;
        }
        ChValue symv = ch_gc_intern_symbol_cstr(&vm->gc, buf);
        int idx = ch_lib_env_intern(out, ch_as_symbol(symv));
        if (idx < 0) {
            snprintf(vm->error, sizeof(vm->error), "import prefix: environment full");
            return -1;
        }
        ch_lib_env_define(out, idx, source.bindings[i].value);
    }
    return 0;
}

static int resolve_import_rename(ChVM *vm, ChValue args, ChLibEnv *out) {
    /* (rename <import-set> (old new) ...) */
    if (!ch_is_pair(args)) {
        snprintf(vm->error, sizeof(vm->error), "import rename: bad syntax");
        return -1;
    }
    ChLibEnv source;
    memset(&source, 0, sizeof(source));
    if (resolve_import_set(vm, ch_car(args), &source) != 0) {
        return -1;
    }

    typedef struct {
        ChSymbol *new_name;
        ChValue value;
    } Pending;
    Pending pending[CH_LIB_ENV_MAX];
    size_t pending_count = 0;

    for (ChValue renames = ch_cdr(args); ch_is_pair(renames); renames = ch_cdr(renames)) {
        ChValue pair = ch_car(renames);
        if (!ch_is_pair(pair) || !ch_is_symbol(ch_car(pair)) || !ch_is_pair(ch_cdr(pair)) ||
            !ch_is_symbol(ch_car(ch_cdr(pair)))) {
            snprintf(vm->error, sizeof(vm->error), "import rename: expected (old new)");
            return -1;
        }
        ChSymbol *old_sym = ch_as_symbol(ch_car(pair));
        ChSymbol *new_sym = ch_as_symbol(ch_car(ch_cdr(pair)));
        int src = env_find_cstr(&source, old_sym->name);
        if (src < 0) {
            snprintf(vm->error, sizeof(vm->error),
                     "import rename: identifier '%s' not found in import set", old_sym->name);
            return -1;
        }
        if (pending_count >= CH_LIB_ENV_MAX) {
            snprintf(vm->error, sizeof(vm->error), "import rename: too many renames");
            return -1;
        }
        pending[pending_count].new_name = new_sym;
        pending[pending_count].value = source.bindings[src].value;
        pending_count++;
        source.bindings[src].defined = false;
    }
    if (merge_env_into_env(out, &source) != 0) {
        return -1;
    }
    for (size_t i = 0; i < pending_count; i++) {
        int idx = ch_lib_env_intern(out, pending[i].new_name);
        if (idx < 0) {
            snprintf(vm->error, sizeof(vm->error), "import rename: environment full");
            return -1;
        }
        ch_lib_env_define(out, idx, pending[i].value);
    }
    return 0;
}

static int resolve_import_set(ChVM *vm, ChValue set, ChLibEnv *out) {
    if (!ch_is_pair(set)) {
        snprintf(vm->error, sizeof(vm->error), "import: expected library name or import set");
        return -1;
    }
    ChValue head = ch_car(set);
    if (ch_is_symbol(head)) {
        const char *h = ch_symbol_basename(ch_as_symbol(head));
        if (strcmp(h, "only") == 0) {
            return resolve_import_only(vm, ch_cdr(set), out);
        }
        if (strcmp(h, "except") == 0) {
            return resolve_import_except(vm, ch_cdr(set), out);
        }
        if (strcmp(h, "prefix") == 0) {
            return resolve_import_prefix(vm, ch_cdr(set), out);
        }
        if (strcmp(h, "rename") == 0) {
            return resolve_import_rename(vm, ch_cdr(set), out);
        }
    }
    ChLibrary *lib = ch_ensure_library(vm, set);
    if (!lib) {
        return -1;
    }
    return library_into_env(out, lib);
}

static ChLibrary *import_set_library(ChVM *vm, ChValue set) {
    if (!ch_is_pair(set)) {
        return NULL;
    }
    ChValue head = ch_car(set);
    if (ch_is_symbol(head)) {
        const char *h = ch_symbol_basename(ch_as_symbol(head));
        if (strcmp(h, "only") == 0 || strcmp(h, "except") == 0 || strcmp(h, "prefix") == 0 ||
            strcmp(h, "rename") == 0) {
            ChValue args = ch_cdr(set);
            if (!ch_is_pair(args)) {
                return NULL;
            }
            return import_set_library(vm, ch_car(args));
        }
    }
    return ch_ensure_library(vm, set);
}

static int process_import_set(ChVM *vm, ChValue set, ChLibEnv *into_env) {
    ChLibEnv scratch;
    memset(&scratch, 0, sizeof(scratch));
    ChLibrary *lib = import_set_library(vm, set);
    if (resolve_import_set(vm, set, &scratch) != 0) {
        return -1;
    }
    if (into_env) {
        return merge_env_into_env(into_env, &scratch);
    }
    return merge_env_into_globals(vm, &scratch, lib ? lib->runtime_env : NULL);
}

int ch_handle_import(ChVM *vm, ChValue args) {
    for (ChValue a = args; ch_is_pair(a); a = ch_cdr(a)) {
        if (process_import_set(vm, ch_car(a), NULL) != 0) {
            return -1;
        }
    }
    if (!ch_is_nil(args) && !ch_is_pair(args)) {
        snprintf(vm->error, sizeof(vm->error), "import: improper list");
        return -1;
    }
    return 0;
}

static void push_lib_env_roots(ChVM *vm, ChLibEnv *env) {
    if (!env) {
        return;
    }
    for (size_t i = 0; i < env->count; i++) {
        if (env->bindings[i].defined) {
            ch_gc_push(&vm->gc, &env->bindings[i].value);
        }
    }
}

static size_t lib_env_root_count(ChLibEnv *env) {
    if (!env) {
        return 0;
    }
    size_t n = 0;
    for (size_t i = 0; i < env->count; i++) {
        if (env->bindings[i].defined) {
            n++;
        }
    }
    return n;
}

static int eval_library_begin(ChVM *vm, ChLibEnv *env, ChValue body) {
    /* Evaluate library bodies in isolated env; do not pollute VM globals. */
    ChLibEnv *saved = vm->active_lib_env;
    vm->active_lib_env = env;

    for (ChValue b = body; ch_is_pair(b); b = ch_cdr(b)) {
        ChValue form = ch_car(b);
        ch_gc_push(&vm->gc, &form);
        /* Root the in-construction env only — it is not yet registered, so
         * ch_library_mark_gc_roots cannot see it. Globals are marked via the VM. */
        push_lib_env_roots(vm, env);
        size_t env_roots = lib_env_root_count(env);

        if (eval_toplevel_form(vm, form) != 0) {
            ch_gc_pop_n(&vm->gc, 1 + env_roots);
            vm->active_lib_env = saved;
            return -1;
        }
        ch_gc_pop_n(&vm->gc, 1 + env_roots);
    }

    vm->active_lib_env = saved;
    return 0;
}

static int process_lib_declaration(ChVM *vm, ChLibEnv *env, ChValue decl,
                                   ChSymbol **export_internal, ChSymbol **export_external,
                                   size_t *export_count);

static int read_forms_from_file(ChVM *vm, const char *path, int fold_case, ChValue *out_list) {
    size_t len = 0;
    char *src = ch_read_file(path, &len);
    if (!src) {
        snprintf(vm->error, sizeof(vm->error), "include: cannot read '%s'", path);
        return -1;
    }
    ChReader reader;
    ch_reader_init(&reader, &vm->gc, src, len);
    reader.fold_case = fold_case;

    ChValue head = CH_NIL;
    ChValue tail = CH_NIL;
    ch_gc_push(&vm->gc, &head);
    ch_gc_push(&vm->gc, &tail);
    int rc = 0;
    for (;;) {
        ChValue expr = CH_NIL;
        ch_gc_push(&vm->gc, &expr);
        ChReadStatus rs = ch_read_datum(&reader, &expr);
        if (rs == CH_READ_EOF) {
            ch_gc_pop(&vm->gc);
            break;
        }
        if (rs != CH_READ_OK) {
            snprintf(vm->error, sizeof(vm->error), "include: read error in '%s': %s", path,
                     ch_reader_error(&reader));
            ch_gc_pop(&vm->gc);
            rc = -1;
            break;
        }
        ChValue cell = ch_gc_cons(&vm->gc, expr, CH_NIL);
        if (ch_is_nil(head)) {
            head = cell;
            tail = cell;
        } else {
            ch_set_cdr(tail, cell);
            tail = cell;
        }
        ch_gc_pop(&vm->gc);
    }
    *out_list = head;
    ch_gc_pop_n(&vm->gc, 2);
    free(src);
    return rc;
}

static int lib_include_exprs(ChVM *vm, ChLibEnv *env, ChValue files, int fold_case) {
    for (ChValue f = files; ch_is_pair(f); f = ch_cdr(f)) {
        if (!ch_is_string(ch_car(f))) {
            snprintf(vm->error, sizeof(vm->error), "include: expected string path");
            return -1;
        }
        ChString *s = ch_as_string(ch_car(f));
        char *path = resolve_include_path(vm, s->data);
        if (!path) {
            snprintf(vm->error, sizeof(vm->error), "include: file not found: %s", s->data);
            return -1;
        }
        char *old_dir = vm->current_lib_dir;
        vm->current_lib_dir = dirname_dup(path);
        ChValue body = CH_NIL;
        ch_gc_push(&vm->gc, &body);
        int rc = read_forms_from_file(vm, path, fold_case, &body);
        free(path);
        if (rc == 0) {
            rc = eval_library_begin(vm, env, body);
        }
        ch_gc_pop(&vm->gc);
        free(vm->current_lib_dir);
        vm->current_lib_dir = old_dir;
        if (rc != 0) {
            return -1;
        }
    }
    return 0;
}

static int lib_include_decls(ChVM *vm, ChLibEnv *env, ChValue files, ChSymbol **export_internal,
                             ChSymbol **export_external, size_t *export_count) {
    for (ChValue f = files; ch_is_pair(f); f = ch_cdr(f)) {
        if (!ch_is_string(ch_car(f))) {
            snprintf(vm->error, sizeof(vm->error),
                     "include-library-declarations: expected string path");
            return -1;
        }
        ChString *s = ch_as_string(ch_car(f));
        char *path = resolve_include_path(vm, s->data);
        if (!path) {
            snprintf(vm->error, sizeof(vm->error),
                     "include-library-declarations: file not found: %s", s->data);
            return -1;
        }
        /* Keep current_lib_dir (library/script dir) for nested ild paths —
         * match Kaappi: do not re-root to the included file's directory. */
        ChValue forms = CH_NIL;
        ch_gc_push(&vm->gc, &forms);
        int rc = read_forms_from_file(vm, path, 0, &forms);
        free(path);
        if (rc == 0) {
            for (ChValue p = forms; ch_is_pair(p); p = ch_cdr(p)) {
                if (process_lib_declaration(vm, env, ch_car(p), export_internal, export_external,
                                            export_count) != 0) {
                    rc = -1;
                    break;
                }
            }
        }
        ch_gc_pop(&vm->gc);
        if (rc != 0) {
            return -1;
        }
    }
    return 0;
}

static int process_lib_declaration(ChVM *vm, ChLibEnv *env, ChValue decl,
                                   ChSymbol **export_internal, ChSymbol **export_external,
                                   size_t *export_count) {
    if (!ch_is_pair(decl) || !ch_is_symbol(ch_car(decl))) {
        snprintf(vm->error, sizeof(vm->error), "define-library: bad declaration");
        return -1;
    }
    const char *tag = ch_symbol_basename(ch_as_symbol(ch_car(decl)));
    ChValue rest = ch_cdr(decl);
    if (strcmp(tag, "export") == 0) {
        for (ChValue e = rest; ch_is_pair(e); e = ch_cdr(e)) {
            ChValue item = ch_car(e);
            if (*export_count >= CH_LIB_MAX_EXPORTS) {
                snprintf(vm->error, sizeof(vm->error), "define-library: too many exports");
                return -1;
            }
            if (ch_is_symbol(item)) {
                export_internal[*export_count] = ch_as_symbol(item);
                export_external[*export_count] = ch_as_symbol(item);
                (*export_count)++;
            } else if (ch_is_pair(item) && ch_is_symbol(ch_car(item)) &&
                       strcmp(ch_symbol_basename(ch_as_symbol(ch_car(item))), "rename") == 0) {
                ChValue r = ch_cdr(item);
                if (!ch_is_pair(r) || !ch_is_symbol(ch_car(r)) || !ch_is_pair(ch_cdr(r)) ||
                    !ch_is_symbol(ch_car(ch_cdr(r)))) {
                    snprintf(vm->error, sizeof(vm->error), "define-library: bad export rename");
                    return -1;
                }
                export_internal[*export_count] = ch_as_symbol(ch_car(r));
                export_external[*export_count] = ch_as_symbol(ch_car(ch_cdr(r)));
                (*export_count)++;
            } else {
                snprintf(vm->error, sizeof(vm->error), "define-library: bad export");
                return -1;
            }
        }
        return 0;
    }
    if (strcmp(tag, "import") == 0) {
        for (ChValue a = rest; ch_is_pair(a); a = ch_cdr(a)) {
            if (process_import_set(vm, ch_car(a), env) != 0) {
                return -1;
            }
        }
        return 0;
    }
    if (strcmp(tag, "begin") == 0) {
        return eval_library_begin(vm, env, rest);
    }
    if (strcmp(tag, "include") == 0) {
        return lib_include_exprs(vm, env, rest, 0);
    }
    if (strcmp(tag, "include-ci") == 0) {
        return lib_include_exprs(vm, env, rest, 1);
    }
    if (strcmp(tag, "include-library-declarations") == 0) {
        return lib_include_decls(vm, env, rest, export_internal, export_external, export_count);
    }
    if (strcmp(tag, "cond-expand") == 0) {
        for (ChValue clauses = rest; ch_is_pair(clauses); clauses = ch_cdr(clauses)) {
            ChValue clause = ch_car(clauses);
            if (!ch_is_pair(clause)) {
                snprintf(vm->error, sizeof(vm->error), "cond-expand: bad clause");
                return -1;
            }
            ChValue req = ch_car(clause);
            int match = 0;
            if (ch_is_symbol(req) && strcmp(ch_symbol_basename(ch_as_symbol(req)), "else") == 0) {
                match = 1;
            } else {
                match = ch_eval_feature_req(vm, req);
            }
            if (match) {
                for (ChValue d = ch_cdr(clause); ch_is_pair(d); d = ch_cdr(d)) {
                    if (process_lib_declaration(vm, env, ch_car(d), export_internal,
                                                export_external, export_count) != 0) {
                        return -1;
                    }
                }
                return 0;
            }
        }
        return 0;
    }
    snprintf(vm->error, sizeof(vm->error), "define-library: unknown declaration '%s'", tag);
    return -1;
}

int ch_handle_define_library(ChVM *vm, ChValue args) {
    if (!ch_is_pair(args)) {
        snprintf(vm->error, sizeof(vm->error), "define-library: bad syntax");
        return -1;
    }
    ChValue name_list = ch_car(args);
    ChValue decls = ch_cdr(args);
    char *dotted = ch_library_name_to_string(name_list);
    if (!dotted) {
        snprintf(vm->error, sizeof(vm->error), "define-library: bad library name");
        return -1;
    }

    ChLibEnv *env = (ChLibEnv *)calloc(1, sizeof(ChLibEnv));
    if (!env) {
        free(dotted);
        return -1;
    }
    ChSymbol *export_internal[CH_LIB_MAX_EXPORTS];
    ChSymbol *export_external[CH_LIB_MAX_EXPORTS];
    size_t export_count = 0;

    for (ChValue d = decls; ch_is_pair(d); d = ch_cdr(d)) {
        if (process_lib_declaration(vm, env, ch_car(d), export_internal, export_external,
                                    &export_count) != 0) {
            free(env);
            free(dotted);
            return -1;
        }
    }

    ChLibrary *lib = (ChLibrary *)calloc(1, sizeof(ChLibrary));
    if (!lib) {
        free(env);
        free(dotted);
        return -1;
    }
    lib->name = dotted;
    lib->runtime_env = env;
    for (size_t i = 0; i < export_count; i++) {
        int idx = ch_lib_env_find(env, export_internal[i]);
        if (idx < 0 || !env->bindings[idx].defined) {
            snprintf(vm->error, sizeof(vm->error),
                     "define-library: exported binding '%s' is undefined",
                     export_internal[i]->name);
            free(lib->name);
            free(lib->runtime_env);
            free(lib);
            return -1;
        }
        lib->export_names[lib->export_count] = export_external[i];
        lib->export_values[lib->export_count] = env->bindings[idx].value;
        lib->export_count++;
    }
    if (ch_library_register(vm->libraries, lib) != 0) {
        snprintf(vm->error, sizeof(vm->error), "define-library: too many libraries");
        free(lib->name);
        free(lib->runtime_env);
        free(lib);
        return -1;
    }
    return 0;
}

int ch_handle_include(ChVM *vm, ChValue args, int fold_case) {
    for (ChValue f = args; ch_is_pair(f); f = ch_cdr(f)) {
        if (!ch_is_string(ch_car(f))) {
            snprintf(vm->error, sizeof(vm->error), "include: expected string path");
            return -1;
        }
        ChString *s = ch_as_string(ch_car(f));
        char *path = resolve_include_path(vm, s->data);
        if (!path) {
            snprintf(vm->error, sizeof(vm->error), "include: file not found: %s", s->data);
            return -1;
        }
        char *old_dir = vm->current_lib_dir;
        vm->current_lib_dir = dirname_dup(path);
        ChValue forms = CH_NIL;
        ch_gc_push(&vm->gc, &forms);
        int rc = read_forms_from_file(vm, path, fold_case, &forms);
        free(path);
        if (rc == 0) {
            for (ChValue p = forms; ch_is_pair(p); p = ch_cdr(p)) {
                if (eval_toplevel_form(vm, ch_car(p)) != 0) {
                    rc = -1;
                    break;
                }
            }
        }
        ch_gc_pop(&vm->gc);
        free(vm->current_lib_dir);
        vm->current_lib_dir = old_dir;
        if (rc != 0) {
            return -1;
        }
    }
    return 0;
}

int ch_cond_expand_select(ChVM *vm, ChValue clauses, ChValue *out_body, char *err, size_t err_len) {
    *out_body = CH_NIL;
    for (ChValue c = clauses; ch_is_pair(c); c = ch_cdr(c)) {
        ChValue clause = ch_car(c);
        if (!ch_is_pair(clause)) {
            snprintf(err, err_len, "cond-expand: bad clause");
            return -1;
        }
        ChValue req = ch_car(clause);
        int match = 0;
        if (ch_is_symbol(req) && strcmp(ch_symbol_basename(ch_as_symbol(req)), "else") == 0) {
            match = 1;
        } else {
            match = ch_eval_feature_req(vm, req);
        }
        if (match) {
            *out_body = ch_cdr(clause);
            return 0;
        }
    }
    return 1;
}

int ch_handle_cond_expand(ChVM *vm, ChValue clauses) {
    ChValue body = CH_NIL;
    char err[256];
    int sel = ch_cond_expand_select(vm, clauses, &body, err, sizeof(err));
    if (sel < 0) {
        snprintf(vm->error, sizeof(vm->error), "%s", err);
        return -1;
    }
    if (sel == 1) {
        return 0;
    }
    for (ChValue b = body; ch_is_pair(b); b = ch_cdr(b)) {
        if (eval_toplevel_form(vm, ch_car(b)) != 0) {
            return -1;
        }
    }
    return 0;
}

static int is_toplevel_keyword(ChValue expr, const char *name) {
    return ch_is_pair(expr) && ch_is_symbol(ch_car(expr)) &&
           strcmp(ch_symbol_basename(ch_as_symbol(ch_car(expr))), name) == 0;
}

static int eval_toplevel_form(ChVM *vm, ChValue expr) {
    if (is_toplevel_keyword(expr, "import")) {
        return ch_handle_import(vm, ch_cdr(expr));
    }
    if (is_toplevel_keyword(expr, "define-library")) {
        return ch_handle_define_library(vm, ch_cdr(expr));
    }
    if (is_toplevel_keyword(expr, "include")) {
        return ch_handle_include(vm, ch_cdr(expr), 0);
    }
    if (is_toplevel_keyword(expr, "include-ci")) {
        return ch_handle_include(vm, ch_cdr(expr), 1);
    }
    if (is_toplevel_keyword(expr, "cond-expand")) {
        return ch_handle_cond_expand(vm, ch_cdr(expr));
    }

    ChCompiler compiler;
    ch_compiler_init(&compiler, vm);
    ChFunction *fn = NULL;
    if (ch_compile_toplevel(&compiler, expr, &fn) != CH_COMPILE_OK) {
        snprintf(vm->error, sizeof(vm->error), "%s", ch_compiler_error(&compiler));
        return -1;
    }
    ChValue result = CH_VOID;
    ch_gc_push(&vm->gc, &result);
    ChVMStatus st = ch_vm_eval_function(vm, fn, &result);
    ch_gc_pop(&vm->gc);
    if (st != CH_VM_OK) {
        return -1;
    }
    return 0;
}
