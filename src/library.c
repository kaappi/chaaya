#include "chaaya/library.h"

#include "chaaya/compiler.h"
#include "chaaya/eval.h"
#include "chaaya/expander.h"
#include "chaaya/features.h"
#include "chaaya/reader.h"

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
                free(existing);
                reg->libs[i] = lib;
                return 0;
            }
        }
    }
    if (reg->count >= CH_LIB_MAX_LIBS) {
        return -1;
    }
    reg->libs[reg->count++] = lib;
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

int ch_register_builtin_libraries(ChVM *vm) {
    if (ch_register_scheme_base_library(vm) != 0) {
        return -1;
    }
    static const char *const write_exports[] = {"display", "write", "newline"};
    static const char *const read_exports[] = {"read", "read-char", "peek-char", "eof-object",
                                               "eof-object?"};
    static const char *const cxr_exports[] = {"caar", "cadr", "cdar", "cddr"};
    static const char *const char_exports[] = {"char?", "char=?", "char<?", "char->integer",
                                               "integer->char"};
    static const char *const process_exports[] = {"exit", "command-line", "features"};
    static const char *const lazy_exports[] = {"delay", "force", "promise?", "make-promise"};
    static const char *const file_exports[] = {
        "call-with-input-file", "call-with-output-file", "with-input-from-file",
        "with-output-to-file",  "open-input-file",       "open-output-file",
        "file-exists?",         "delete-file"};
    static const char *const complex_exports[] = {
        "angle", "imag-part", "magnitude", "make-polar", "make-rectangular", "real-part"};
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
    /* case-lambda is a compiler special form; library exists for R7RS import. */
    if (register_exports_from_globals(vm, "scheme.case-lambda", NULL, 0) != 0) {
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

static int merge_env_into_globals(ChVM *vm, const ChLibEnv *env) {
    for (size_t i = 0; i < env->count; i++) {
        if (!env->bindings[i].defined) {
            continue;
        }
        int idx = ch_vm_intern_global(vm, env->bindings[i].name);
        ch_vm_define_global(vm, idx, env->bindings[i].value);
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
    }
    return n;
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
        for (size_t i = 0; i < vm->global_count; i++) {
            ch_gc_push(&vm->gc, &vm->globals[i].value);
        }
        ChReadStatus rs = ch_read_datum(&reader, &expr);
        ch_gc_pop_n(&vm->gc, vm->global_count);
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
    if (loading_push(vm, dotted) != 0) {
        free(dotted);
        return NULL;
    }
    char *rel = ch_library_name_to_path(name_list);
    if (!rel) {
        snprintf(vm->error, sizeof(vm->error), "import: bad library name");
        loading_pop(vm);
        free(dotted);
        return NULL;
    }
    char *path = resolve_library_path(vm, rel);
    free(rel);
    if (!path) {
        snprintf(vm->error, sizeof(vm->error), "library not found: %s", dotted);
        loading_pop(vm);
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

static int process_import_set(ChVM *vm, ChValue set, ChLibEnv *into_env) {
    ChLibEnv scratch;
    memset(&scratch, 0, sizeof(scratch));
    if (resolve_import_set(vm, set, &scratch) != 0) {
        return -1;
    }
    if (into_env) {
        return merge_env_into_env(into_env, &scratch);
    }
    return merge_env_into_globals(vm, &scratch);
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

static int eval_library_begin(ChVM *vm, ChLibEnv *env, ChValue body) {
    /* MVP: evaluate library bodies against real globals so closures keep
     * working after the library is registered. Imports are merged into
     * globals first; defines land in globals and are snapshotted into env. */
    for (size_t i = 0; i < env->count; i++) {
        if (env->bindings[i].defined) {
            int g = ch_vm_intern_global(vm, env->bindings[i].name);
            ch_vm_define_global(vm, g, env->bindings[i].value);
        }
    }

    size_t globals_before = vm->global_count;
    /* Track which names existed so we can detect new defines — actually we
     * re-scan exports from globals by name at the end. For env snapshot of
     * all defines during begin, record global indices after each form. */

    for (ChValue b = body; ch_is_pair(b); b = ch_cdr(b)) {
        ChValue form = ch_car(b);
        ch_gc_push(&vm->gc, &form);
        for (size_t i = 0; i < vm->global_count; i++) {
            ch_gc_push(&vm->gc, &vm->globals[i].value);
        }
        size_t groots = vm->global_count;

        if (eval_toplevel_form(vm, form) != 0) {
            ch_gc_pop_n(&vm->gc, 1 + groots);
            return -1;
        }
        ch_gc_pop_n(&vm->gc, 1 + groots);

        /* Sync any newly defined globals into env for export lookup. */
        for (size_t i = 0; i < vm->global_count; i++) {
            if (!vm->globals[i].defined) {
                continue;
            }
            int idx = ch_lib_env_intern(env, vm->globals[i].name);
            if (idx < 0) {
                snprintf(vm->error, sizeof(vm->error), "define-library: environment full");
                return -1;
            }
            ch_lib_env_define(env, idx, vm->globals[i].value);
        }
    }
    (void)globals_before;
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
        char *old_dir = vm->current_lib_dir;
        vm->current_lib_dir = dirname_dup(path);
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
        free(vm->current_lib_dir);
        vm->current_lib_dir = old_dir;
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

    ChLibEnv env;
    memset(&env, 0, sizeof(env));
    ChSymbol *export_internal[CH_LIB_MAX_EXPORTS];
    ChSymbol *export_external[CH_LIB_MAX_EXPORTS];
    size_t export_count = 0;

    for (ChValue d = decls; ch_is_pair(d); d = ch_cdr(d)) {
        if (process_lib_declaration(vm, &env, ch_car(d), export_internal, export_external,
                                    &export_count) != 0) {
            free(dotted);
            return -1;
        }
    }

    ChLibrary *lib = (ChLibrary *)calloc(1, sizeof(ChLibrary));
    if (!lib) {
        free(dotted);
        return -1;
    }
    lib->name = dotted;
    for (size_t i = 0; i < export_count; i++) {
        int idx = ch_lib_env_find(&env, export_internal[i]);
        if (idx < 0 || !env.bindings[idx].defined) {
            snprintf(vm->error, sizeof(vm->error),
                     "define-library: exported binding '%s' is undefined",
                     export_internal[i]->name);
            free(lib->name);
            free(lib);
            return -1;
        }
        lib->export_names[lib->export_count] = export_external[i];
        lib->export_values[lib->export_count] = env.bindings[idx].value;
        lib->export_count++;
    }
    if (ch_library_register(vm->libraries, lib) != 0) {
        snprintf(vm->error, sizeof(vm->error), "define-library: too many libraries");
        free(lib->name);
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

int ch_handle_cond_expand(ChVM *vm, ChValue clauses) {
    for (ChValue c = clauses; ch_is_pair(c); c = ch_cdr(c)) {
        ChValue clause = ch_car(c);
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
            for (ChValue b = ch_cdr(clause); ch_is_pair(b); b = ch_cdr(b)) {
                if (eval_toplevel_form(vm, ch_car(b)) != 0) {
                    return -1;
                }
            }
            return 0;
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
