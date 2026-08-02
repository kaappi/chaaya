#include "chaaya/coverage.h"

#include <stdio.h>
#include <string.h>

#define CH_COVERAGE_MAX 256

typedef struct ChCoverageEntry {
    char library[96];
    char name[96];
    unsigned hits;
} ChCoverageEntry;

static int g_enabled;
static ChCoverageEntry g_entries[CH_COVERAGE_MAX];
static size_t g_count;

void ch_coverage_enable(void) {
    g_enabled = 1;
    g_count = 0;
    memset(g_entries, 0, sizeof(g_entries));
}

int ch_coverage_enabled(void) {
    return g_enabled;
}

void ch_coverage_register(const char *library, const char *name) {
    if (!g_enabled || !library || !name) {
        return;
    }
    for (size_t i = 0; i < g_count; i++) {
        if (strcmp(g_entries[i].library, library) == 0 && strcmp(g_entries[i].name, name) == 0) {
            return;
        }
    }
    if (g_count >= CH_COVERAGE_MAX) {
        return;
    }
    ChCoverageEntry *e = &g_entries[g_count++];
    snprintf(e->library, sizeof(e->library), "%s", library);
    snprintf(e->name, sizeof(e->name), "%s", name);
    e->hits = 0;
}

void ch_coverage_hit(const char *library, const char *name) {
    if (!g_enabled || !library || !name) {
        return;
    }
    for (size_t i = 0; i < g_count; i++) {
        if (strcmp(g_entries[i].library, library) == 0 && strcmp(g_entries[i].name, name) == 0) {
            g_entries[i].hits++;
            return;
        }
    }
    if (g_count >= CH_COVERAGE_MAX) {
        return;
    }
    ChCoverageEntry *e = &g_entries[g_count++];
    snprintf(e->library, sizeof(e->library), "%s", library);
    snprintf(e->name, sizeof(e->name), "%s", name);
    e->hits = 1;
}

void ch_coverage_report_text(FILE *out) {
    if (!g_enabled || !out) {
        return;
    }
    fprintf(out, "Coverage:\n");
    unsigned touched = 0;
    unsigned total = (unsigned)g_count;
    for (size_t i = 0; i < g_count; i++) {
        if (g_entries[i].hits > 0) {
            touched++;
        }
        fprintf(out, "  (%s) %s  hits=%u%s\n", g_entries[i].library, g_entries[i].name,
                g_entries[i].hits, g_entries[i].hits == 0 ? "  [uncalled]" : "");
    }
    fprintf(out, "Overall: %u/%u procedures covered\n", touched, total);
}

int ch_coverage_report_xml(const char *path) {
    if (!g_enabled || !path) {
        return -1;
    }
    FILE *f = fopen(path, "w");
    if (!f) {
        return -1;
    }
    fprintf(f, "<?xml version=\"1.0\" ?>\n");
    fprintf(f, "<coverage version=\"1\">\n");
    for (size_t i = 0; i < g_count; i++) {
        fprintf(f,
                "  <procedure library=\"%s\" name=\"%s\" hits=\"%u\"/>\n",
                g_entries[i].library, g_entries[i].name, g_entries[i].hits);
    }
    fprintf(f, "</coverage>\n");
    fclose(f);
    return 0;
}
