#include "chaaya/library.h"

#include <stdlib.h>
#include <string.h>

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
    static const char *const cxr_exports[] = {
        "caar",   "cadr",   "cdar",   "cddr",   "caaar",  "caadr",  "cadar",  "caddr",
        "cdaar",  "cdadr",  "cddar",  "cdddr",  "caaaar", "caaadr", "caadar", "caaddr",
        "cadaar", "cadadr", "caddar", "cadddr", "cdaaar", "cdaadr", "cdadar", "cdaddr",
        "cddaar", "cddadr", "cdddar", "cddddr",
    };
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
        "channel-timeout-exception?", "processor-count"};
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
                                                      "ffi-callback?",
                                                      "ffi-bytevector-ptr"};
    static const char *const chaaya_primitives_exports[] = {
        "%default-random-source", "%rs-next-int", "%rs-next-real"};
    static const char *const srfi170_exports[] = {
        "directory-files",         "file-info",           "file-info?",
        "file-info-directory?",    "file-info-regular?",  "file-info-symlink?",
        "file-info:size",          "file-info:mtime",     "file-info:mode",
        "file-info:blocks",        "file-info-type",      "temp-file-prefix",    "create-temp-file",
        "create-directory",        "delete-directory",    "rename-file",
        "real-path",               "current-directory",   "set-current-directory!",
        "file-exists?",            "delete-file"};
    static const char *const srfi18_exports[] = {
        "make-thread",      "thread-start!",    "thread-join!",     "thread-sleep!",
        "thread-yield!",    "current-thread",   "thread?",          "thread-name",
        "thread-specific",  "thread-specific-set!", "thread-terminate!",
        "make-mutex",       "mutex?",           "mutex-lock!",      "mutex-unlock!",
        "mutex-name",       "mutex-specific",   "mutex-specific-set!", "mutex-state",
        "make-condition-variable", "condition-variable?",
        "condition-variable-signal!", "condition-variable-broadcast!",
        "condition-variable-name", "condition-variable-specific",
        "condition-variable-specific-set!",
        "current-time",     "time->seconds",    "seconds->time",
        "join-timeout-exception?", "terminated-thread-exception?",
        "abandoned-mutex-exception?", "uncaught-exception?", "uncaught-exception-reason"};
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
        "string-take-right", "string-drop-right", "string-pad", "string-pad-right",
        "string-replace", "string-titlecase", "string-join", "string-split", "string-tabulate",
        "string-trim", "string-trim-right", "string-trim-both",
        "string-every", "string-any", "string-count", "string-filter", "string-delete",
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
        "vector-fold", "vector-fold-right", "vector-cumulate", "vector-map!", "vector-partition",
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
