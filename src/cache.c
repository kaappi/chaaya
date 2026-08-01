#include "chaaya/cache.h"

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

int ch_cache_status(void) {
    char cache_dir[PATH_MAX];
    if (resolve_cache_dir(cache_dir, sizeof(cache_dir)) != 0) {
        fprintf(stderr, "cache: cannot resolve cache path (set HOME or CHAAYA_HOME)\n");
        return 1;
    }

    printf("Bytecode cache writer: not implemented yet\n");
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
