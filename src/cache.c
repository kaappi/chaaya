#include "chaaya/cache.h"

#include "chaaya/gc.h"
#include "chaaya/value.h"
#include "chaaya/version.h"

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define CH_CACHE_MAGIC "CHBC"
#define CH_CACHE_VERSION 3u

typedef struct ChCacheStats {
    size_t entries;
    uint64_t bytes;
} ChCacheStats;

static int resolve_cache_dir(char *out, size_t out_len) {
    const char *base = getenv("CHAAYA_HOME");
    if (base && base[0] != '\0') {
        return snprintf(out, out_len, "%s/cache", base) < (int)out_len ? 0 : -1;
    }

    const char *home = getenv("HOME");
    if (!home || home[0] == '\0') {
        return -1;
    }
    return snprintf(out, out_len, "%s/.chaaya/cache", home) < (int)out_len ? 0 : -1;
}

static bool is_dot_entry(const char *name) {
    return strcmp(name, ".") == 0 || strcmp(name, "..") == 0;
}

static int build_child_path(char *out, size_t out_len, const char *dir, const char *name) {
    return snprintf(out, out_len, "%s/%s", dir, name) < (int)out_len ? 0 : -1;
}

static int collect_cache_stats(const char *dir, ChCacheStats *stats) {
    DIR *dp = opendir(dir);
    if (!dp) {
        return -1;
    }

    int rc = 0;
    struct dirent *ent = NULL;
    while ((ent = readdir(dp)) != NULL) {
        if (is_dot_entry(ent->d_name)) {
            continue;
        }

        char path[PATH_MAX];
        if (build_child_path(path, sizeof(path), dir, ent->d_name) != 0) {
            rc = -1;
            break;
        }

        struct stat st;
        if (lstat(path, &st) != 0) {
            rc = -1;
            break;
        }

        if (S_ISDIR(st.st_mode)) {
            if (collect_cache_stats(path, stats) != 0) {
                rc = -1;
                break;
            }
            continue;
        }

        stats->entries++;
        if (S_ISREG(st.st_mode)) {
            stats->bytes += (uint64_t)st.st_size;
        }
    }

    closedir(dp);
    return rc;
}

static int clear_cache_dir(const char *dir, size_t *removed_entries) {
    DIR *dp = opendir(dir);
    if (!dp) {
        return -1;
    }

    int rc = 0;
    struct dirent *ent = NULL;
    while ((ent = readdir(dp)) != NULL) {
        if (is_dot_entry(ent->d_name)) {
            continue;
        }

        char path[PATH_MAX];
        if (build_child_path(path, sizeof(path), dir, ent->d_name) != 0) {
            rc = -1;
            break;
        }

        struct stat st;
        if (lstat(path, &st) != 0) {
            rc = -1;
            break;
        }

        if (S_ISDIR(st.st_mode)) {
            if (clear_cache_dir(path, removed_entries) != 0) {
                rc = -1;
                break;
            }
            if (rmdir(path) != 0) {
                rc = -1;
                break;
            }
            continue;
        }

        if (unlink(path) != 0) {
            rc = -1;
            break;
        }
        (*removed_entries)++;
    }

    closedir(dp);
    return rc;
}

static uint64_t fnv1a64(const void *data, size_t len) {
    const unsigned char *p = (const unsigned char *)data;
    uint64_t h = 14695981039346656037ull;
    for (size_t i = 0; i < len; i++) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}

static int ensure_cache_dir(char *cache_dir, size_t cache_dir_len) {
    if (resolve_cache_dir(cache_dir, cache_dir_len) != 0) {
        return -1;
    }
    struct stat st;
    if (stat(cache_dir, &st) == 0) {
        return S_ISDIR(st.st_mode) ? 0 : -1;
    }
    if (errno != ENOENT) {
        return -1;
    }
    /* mkdir parents: ~/.chaaya then cache */
    char parent[PATH_MAX];
    snprintf(parent, sizeof(parent), "%s", cache_dir);
    char *slash = strrchr(parent, '/');
    if (slash && slash != parent) {
        *slash = '\0';
        if (mkdir(parent, 0755) != 0 && errno != EEXIST) {
            return -1;
        }
    }
    if (mkdir(cache_dir, 0755) != 0 && errno != EEXIST) {
        return -1;
    }
    return 0;
}

static void cache_path_for(const char *abs_key, char *out, size_t out_len) {
    char dir[PATH_MAX];
    if (ensure_cache_dir(dir, sizeof(dir)) != 0) {
        out[0] = '\0';
        return;
    }
    uint64_t h = fnv1a64(abs_key, strlen(abs_key));
    snprintf(out, out_len, "%s/%016llx.chbc", dir, (unsigned long long)h);
}

static int write_u32(FILE *f, uint32_t v) {
    unsigned char b[4] = {(unsigned char)(v), (unsigned char)(v >> 8), (unsigned char)(v >> 16),
                          (unsigned char)(v >> 24)};
    return fwrite(b, 1, 4, f) == 4 ? 0 : -1;
}

static int write_u64(FILE *f, uint64_t v) {
    for (int i = 0; i < 8; i++) {
        unsigned char b = (unsigned char)(v >> (8 * i));
        if (fwrite(&b, 1, 1, f) != 1) {
            return -1;
        }
    }
    return 0;
}

static int write_blob(FILE *f, const void *p, size_t n) {
    if (write_u32(f, (uint32_t)n) != 0) {
        return -1;
    }
    if (n == 0) {
        return 0;
    }
    return fwrite(p, 1, n, f) == n ? 0 : -1;
}

static int read_u32(FILE *f, uint32_t *v) {
    unsigned char b[4];
    if (fread(b, 1, 4, f) != 4) {
        return -1;
    }
    *v = (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
    return 0;
}

static int read_u64(FILE *f, uint64_t *v) {
    uint64_t x = 0;
    for (int i = 0; i < 8; i++) {
        unsigned char b;
        if (fread(&b, 1, 1, f) != 1) {
            return -1;
        }
        x |= (uint64_t)b << (8 * i);
    }
    *v = x;
    return 0;
}

static int read_blob(FILE *f, char **out, size_t *out_len) {
    uint32_t n = 0;
    if (read_u32(f, &n) != 0) {
        return -1;
    }
    char *buf = (char *)malloc(n + 1);
    if (!buf) {
        return -1;
    }
    if (n > 0 && fread(buf, 1, n, f) != n) {
        free(buf);
        return -1;
    }
    buf[n] = '\0';
    *out = buf;
    *out_len = n;
    return 0;
}

/* Serialize a function with immediate/string/symbol constants only.
 * Nested function constants are stored recursively. Other heap types abort store. */
static int write_function(FILE *f, ChFunction *fn) {
    if (!fn) {
        return -1;
    }
    if (write_u32(f, fn->arity) != 0 || write_u32(f, fn->num_regs) != 0 ||
        write_u32(f, fn->num_upvalues) != 0 || write_u32(f, fn->variadic) != 0) {
        return -1;
    }
    if (write_blob(f, fn->code, fn->code_len) != 0) {
        return -1;
    }
    if (write_u32(f, (uint32_t)fn->const_count) != 0) {
        return -1;
    }
    for (size_t i = 0; i < fn->const_count; i++) {
        ChValue c = fn->constants[i];
        if (ch_is_fixnum(c) || ch_is_nil(c) || c == CH_TRUE || c == CH_FALSE || c == CH_VOID ||
            c == CH_EOF_OBJ || ch_is_char(c) || ch_is_flonum(c)) {
            if (fputc('I', f) == EOF) {
                return -1;
            }
            if (fwrite(&c, sizeof(c), 1, f) != 1) {
                return -1;
            }
        } else if (ch_is_string(c)) {
            ChString *s = ch_as_string(c);
            if (fputc('S', f) == EOF) {
                return -1;
            }
            if (write_blob(f, s->data, s->len) != 0) {
                return -1;
            }
        } else if (ch_is_symbol(c)) {
            const char *name = ch_as_symbol(c)->name;
            if (fputc('Y', f) == EOF) {
                return -1;
            }
            if (write_blob(f, name, strlen(name)) != 0) {
                return -1;
            }
        } else if (ch_is_function(c)) {
            if (fputc('F', f) == EOF) {
                return -1;
            }
            if (write_function(f, ch_as_function(c)) != 0) {
                return -1;
            }
        } else {
            /* Unsupported constant — refuse to cache this function. */
            return -1;
        }
    }
    if (fn->num_upvalues > 0) {
        if (fwrite(fn->uv_is_local, 1, fn->num_upvalues, f) != fn->num_upvalues) {
            return -1;
        }
        if (fwrite(fn->uv_index, 1, fn->num_upvalues, f) != fn->num_upvalues) {
            return -1;
        }
    }
    return 0;
}

static ChFunction *read_function(ChVM *vm, FILE *f);

static ChFunction *read_function(ChVM *vm, FILE *f) {
    uint32_t arity = 0, num_regs = 0, num_upvalues = 0, variadic = 0;
    if (read_u32(f, &arity) != 0 || read_u32(f, &num_regs) != 0 || read_u32(f, &num_upvalues) != 0 ||
        read_u32(f, &variadic) != 0) {
        return NULL;
    }
    char *code = NULL;
    size_t code_len = 0;
    if (read_blob(f, &code, &code_len) != 0) {
        return NULL;
    }
    uint32_t const_count = 0;
    if (read_u32(f, &const_count) != 0) {
        free(code);
        return NULL;
    }

    ChValue fn_v = ch_gc_make_function(&vm->gc);
    ChFunction *fn = ch_as_function(fn_v);
    ch_gc_push(&vm->gc, &fn_v);

    fn->arity = (uint8_t)arity;
    fn->num_regs = (uint8_t)num_regs;
    fn->num_upvalues = (uint8_t)num_upvalues;
    fn->variadic = (uint8_t)variadic;
    fn->code = (uint8_t *)malloc(code_len ? code_len : 1);
    if (!fn->code) {
        free(code);
        ch_gc_pop(&vm->gc);
        return NULL;
    }
    memcpy(fn->code, code, code_len);
    fn->code_len = code_len;
    free(code);

    if (const_count > 0) {
        fn->constants = (ChValue *)calloc(const_count, sizeof(ChValue));
        if (!fn->constants) {
            ch_gc_pop(&vm->gc);
            return NULL;
        }
        fn->const_count = const_count;
        for (uint32_t i = 0; i < const_count; i++) {
            int tag = fgetc(f);
            if (tag == 'I') {
                ChValue c;
                if (fread(&c, sizeof(c), 1, f) != 1) {
                    ch_gc_pop(&vm->gc);
                    return NULL;
                }
                fn->constants[i] = c;
            } else if (tag == 'S') {
                char *s = NULL;
                size_t sl = 0;
                if (read_blob(f, &s, &sl) != 0) {
                    ch_gc_pop(&vm->gc);
                    return NULL;
                }
                fn->constants[i] = ch_gc_make_string(&vm->gc, s, sl);
                free(s);
            } else if (tag == 'Y') {
                char *s = NULL;
                size_t sl = 0;
                if (read_blob(f, &s, &sl) != 0) {
                    ch_gc_pop(&vm->gc);
                    return NULL;
                }
                fn->constants[i] = ch_gc_intern_symbol_cstr(&vm->gc, s);
                free(s);
            } else if (tag == 'F') {
                ChFunction *inner = read_function(vm, f);
                if (!inner) {
                    ch_gc_pop(&vm->gc);
                    return NULL;
                }
                fn->constants[i] = ch_make_pointer(&inner->header);
            } else {
                ch_gc_pop(&vm->gc);
                return NULL;
            }
        }
    }

    if (num_upvalues > 0) {
        fn->uv_is_local = (uint8_t *)calloc(num_upvalues, 1);
        fn->uv_index = (uint8_t *)calloc(num_upvalues, 1);
        if (!fn->uv_is_local || !fn->uv_index ||
            fread(fn->uv_is_local, 1, num_upvalues, f) != num_upvalues ||
            fread(fn->uv_index, 1, num_upvalues, f) != num_upvalues) {
            ch_gc_pop(&vm->gc);
            return NULL;
        }
    }

    ch_gc_pop(&vm->gc);
    return fn;
}

static bool cache_disabled(void) {
    const char *v = getenv("CHAAYA_NO_CACHE");
    return v && v[0] != '\0' && strcmp(v, "0") != 0;
}

int ch_cache_store(ChVM *vm, const char *path, const char *source, size_t source_len,
                   ChFunction **fns, size_t count) {
    if (cache_disabled()) {
        return -1;
    }
    if (!vm || !path || !source || !fns) {
        return -1;
    }
    char abs[PATH_MAX];
    if (!realpath(path, abs)) {
        snprintf(abs, sizeof(abs), "%s", path);
    }
    char cpath[PATH_MAX];
    cache_path_for(abs, cpath, sizeof(cpath));
    if (!cpath[0]) {
        return -1;
    }

    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s.tmp", cpath);
    FILE *f = fopen(tmp, "wb");
    if (!f) {
        return -1;
    }

    uint64_t src_hash = fnv1a64(source, source_len);
    uint64_t ver_hash = fnv1a64(CHAAYA_VERSION, strlen(CHAAYA_VERSION));
    int ok = 1;
    if (fwrite(CH_CACHE_MAGIC, 1, 4, f) != 4) {
        ok = 0;
    }
    if (ok && write_u32(f, CH_CACHE_VERSION) != 0) {
        ok = 0;
    }
    if (ok && write_u64(f, src_hash) != 0) {
        ok = 0;
    }
    if (ok && write_u64(f, ver_hash) != 0) {
        ok = 0;
    }
    if (ok && write_blob(f, abs, strlen(abs)) != 0) {
        ok = 0;
    }
    if (ok && write_u32(f, (uint32_t)count) != 0) {
        ok = 0;
    }
    for (size_t i = 0; ok && i < count; i++) {
        if (write_function(f, fns[i]) != 0) {
            ok = 0;
        }
    }
    /* Global name table: bytecode uses absolute indices into vm->globals. */
    if (ok && write_u32(f, (uint32_t)vm->global_count) != 0) {
        ok = 0;
    }
    for (size_t i = 0; ok && i < vm->global_count; i++) {
        const char *name = vm->globals[i].name ? vm->globals[i].name->name : "";
        if (write_blob(f, name, strlen(name)) != 0) {
            ok = 0;
        }
    }
    fclose(f);
    if (!ok) {
        unlink(tmp);
        return -1;
    }
    if (rename(tmp, cpath) != 0) {
        unlink(tmp);
        return -1;
    }
    return 0;
}

int ch_cache_try_load(ChVM *vm, const char *path, const char *source, size_t source_len,
                      ChFunction ***out_fns, size_t *out_count) {
    if (!vm || !path || !source || !out_fns || !out_count) {
        return 0;
    }
    if (cache_disabled()) {
        return 0;
    }
    char abs[PATH_MAX];
    if (!realpath(path, abs)) {
        snprintf(abs, sizeof(abs), "%s", path);
    }
    char cpath[PATH_MAX];
    cache_path_for(abs, cpath, sizeof(cpath));
    if (!cpath[0]) {
        return 0;
    }
    FILE *f = fopen(cpath, "rb");
    if (!f) {
        return 0;
    }
    char magic[4];
    if (fread(magic, 1, 4, f) != 4 || memcmp(magic, CH_CACHE_MAGIC, 4) != 0) {
        fclose(f);
        return 0;
    }
    uint32_t ver = 0;
    uint64_t src_hash = 0, ver_hash = 0;
    if (read_u32(f, &ver) != 0 || ver != CH_CACHE_VERSION || read_u64(f, &src_hash) != 0 ||
        read_u64(f, &ver_hash) != 0) {
        fclose(f);
        return 0;
    }
    if (src_hash != fnv1a64(source, source_len) ||
        ver_hash != fnv1a64(CHAAYA_VERSION, strlen(CHAAYA_VERSION))) {
        fclose(f);
        return 0;
    }
    char *stored_path = NULL;
    size_t plen = 0;
    if (read_blob(f, &stored_path, &plen) != 0) {
        fclose(f);
        return 0;
    }
    free(stored_path);
    uint32_t count = 0;
    if (read_u32(f, &count) != 0) {
        fclose(f);
        return 0;
    }
    ChFunction **fns = (ChFunction **)calloc(count ? count : 1, sizeof(ChFunction *));
    ChValue *roots = (ChValue *)calloc(count ? count : 1, sizeof(ChValue));
    if (!fns || !roots) {
        free(fns);
        free(roots);
        fclose(f);
        return 0;
    }
    size_t rooted = 0;
    for (uint32_t i = 0; i < count; i++) {
        fns[i] = read_function(vm, f);
        if (!fns[i]) {
            ch_gc_pop_n(&vm->gc, rooted);
            free(fns);
            free(roots);
            fclose(f);
            return 0;
        }
        /* Keep earlier top-level functions alive while later ones allocate. */
        roots[i] = ch_make_pointer(&fns[i]->header);
        ch_gc_push(&vm->gc, &roots[i]);
        rooted++;
    }
    /* Restore global name slots so DEFINE/GET/SET indices match compile time. */
    uint32_t gcount = 0;
    if (read_u32(f, &gcount) != 0) {
        ch_gc_pop_n(&vm->gc, rooted);
        free(fns);
        free(roots);
        fclose(f);
        return 0;
    }
    for (uint32_t i = 0; i < gcount; i++) {
        char *name = NULL;
        size_t nlen = 0;
        if (read_blob(f, &name, &nlen) != 0) {
            ch_gc_pop_n(&vm->gc, rooted);
            free(fns);
            free(roots);
            fclose(f);
            return 0;
        }
        if (i < vm->global_count) {
            const char *have = vm->globals[i].name ? vm->globals[i].name->name : "";
            if (strcmp(have, name) != 0) {
                free(name);
                ch_gc_pop_n(&vm->gc, rooted);
                free(fns);
                free(roots);
                fclose(f);
                return 0;
            }
        } else {
            ChValue symv = ch_gc_intern_symbol_cstr(&vm->gc, name);
            int idx = ch_vm_intern_global(vm, ch_as_symbol(symv));
            if (idx < 0 || (uint32_t)idx != i) {
                free(name);
                ch_gc_pop_n(&vm->gc, rooted);
                free(fns);
                free(roots);
                fclose(f);
                return 0;
            }
        }
        free(name);
    }

    fclose(f);
    ch_gc_pop_n(&vm->gc, rooted);
    free(roots);
    *out_fns = fns;
    *out_count = count;
    return 1;
}

int ch_cache_status(void) {
    char cache_dir[PATH_MAX];
    if (resolve_cache_dir(cache_dir, sizeof(cache_dir)) != 0) {
        fprintf(stderr, "cache: cannot resolve cache path (set HOME or CHAAYA_HOME)\n");
        return 1;
    }

    printf("Bytecode cache: enabled (.chbc)\n");
    printf("Location: %s\n", cache_dir);

    struct stat st;
    if (stat(cache_dir, &st) != 0) {
        if (errno == ENOENT) {
            printf("Entries: 0\n");
            printf("Size: 0 bytes\n");
            return 0;
        }
        fprintf(stderr, "cache: cannot access '%s': %s\n", cache_dir, strerror(errno));
        return 1;
    }
    if (!S_ISDIR(st.st_mode)) {
        fprintf(stderr, "cache: path is not a directory: %s\n", cache_dir);
        return 1;
    }

    ChCacheStats stats = {0};
    if (collect_cache_stats(cache_dir, &stats) != 0) {
        fprintf(stderr, "cache: failed to inspect '%s': %s\n", cache_dir, strerror(errno));
        return 1;
    }

    printf("Entries: %zu\n", stats.entries);
    printf("Size: %llu bytes\n", (unsigned long long)stats.bytes);
    return 0;
}

int ch_cache_clear(void) {
    char cache_dir[PATH_MAX];
    if (resolve_cache_dir(cache_dir, sizeof(cache_dir)) != 0) {
        fprintf(stderr, "cache: cannot resolve cache path (set HOME or CHAAYA_HOME)\n");
        return 1;
    }

    struct stat st;
    if (stat(cache_dir, &st) != 0) {
        if (errno == ENOENT) {
            printf("Bytecode cache cleared: 0 entries removed\n");
            printf("Location: %s\n", cache_dir);
            return 0;
        }
        fprintf(stderr, "cache: cannot access '%s': %s\n", cache_dir, strerror(errno));
        return 1;
    }
    if (!S_ISDIR(st.st_mode)) {
        fprintf(stderr, "cache: path is not a directory: %s\n", cache_dir);
        return 1;
    }

    size_t removed = 0;
    if (clear_cache_dir(cache_dir, &removed) != 0) {
        fprintf(stderr, "cache: failed to clear '%s': %s\n", cache_dir, strerror(errno));
        return 1;
    }

    printf("Bytecode cache cleared: %zu entries removed\n", removed);
    printf("Location: %s\n", cache_dir);
    return 0;
}
