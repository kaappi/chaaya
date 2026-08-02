#include "chaaya/profile.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define CH_PROFILE_MAX 64

typedef struct ChProfileEntry {
    char name[64];
    uint64_t calls;
    uint64_t total_ns;
    uint64_t active_ns;
    int depth;
} ChProfileEntry;

static int g_enabled;
static ChProfileEntry g_entries[CH_PROFILE_MAX];
static size_t g_count;

static uint64_t now_ns(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

void ch_profile_enable(void) {
    g_enabled = 1;
    g_count = 0;
    memset(g_entries, 0, sizeof(g_entries));
}

int ch_profile_enabled(void) {
    return g_enabled;
}

static ChProfileEntry *find_or_add(const char *name) {
    for (size_t i = 0; i < g_count; i++) {
        if (strcmp(g_entries[i].name, name) == 0) {
            return &g_entries[i];
        }
    }
    if (g_count >= CH_PROFILE_MAX) {
        return NULL;
    }
    ChProfileEntry *e = &g_entries[g_count++];
    snprintf(e->name, sizeof(e->name), "%s", name);
    return e;
}

void ch_profile_enter(const char *name) {
    if (!g_enabled || !name) {
        return;
    }
    ChProfileEntry *e = find_or_add(name);
    if (!e) {
        return;
    }
    e->calls++;
    e->depth++;
    if (e->depth == 1) {
        e->active_ns = now_ns();
    }
}

void ch_profile_leave(const char *name) {
    if (!g_enabled || !name) {
        return;
    }
    for (size_t i = 0; i < g_count; i++) {
        if (strcmp(g_entries[i].name, name) == 0) {
            ChProfileEntry *e = &g_entries[i];
            if (e->depth > 0) {
                e->depth--;
                if (e->depth == 0) {
                    uint64_t t = now_ns();
                    if (t >= e->active_ns) {
                        e->total_ns += t - e->active_ns;
                    }
                }
            }
            return;
        }
    }
}

void ch_profile_report_text(FILE *out) {
    if (!g_enabled || !out) {
        return;
    }
    fprintf(out, "Profile:\n");
    for (size_t i = 0; i < g_count; i++) {
        fprintf(out, "  %-32s  calls=%llu  %.3fms\n", g_entries[i].name,
                (unsigned long long)g_entries[i].calls,
                (double)g_entries[i].total_ns / 1000000.0);
    }
}

int ch_profile_report_json(const char *path) {
    if (!g_enabled || !path) {
        return -1;
    }
    FILE *f = fopen(path, "w");
    if (!f) {
        return -1;
    }
    fprintf(f, "{\"entries\":[");
    for (size_t i = 0; i < g_count; i++) {
        if (i) {
            fputc(',', f);
        }
        fprintf(f, "{\"name\":\"%s\",\"calls\":%llu,\"ms\":%.3f}", g_entries[i].name,
                (unsigned long long)g_entries[i].calls,
                (double)g_entries[i].total_ns / 1000000.0);
    }
    fprintf(f, "]}\n");
    fclose(f);
    return 0;
}
