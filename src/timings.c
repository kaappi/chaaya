#include "chaaya/timings.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define CH_TIMINGS_STACK_MAX 128
#define CH_TIMINGS_PATH_MAX 4096

typedef struct ChTimingsFrame {
    ChTimingsStage stage;
    uint64_t resumed_ns;
} ChTimingsFrame;

static int g_enabled;
static ChTimingsFormat g_format;
static ChTimingsMode g_mode;
static uint64_t g_buckets[CH_TIMINGS_STAGE_COUNT];
static ChTimingsFrame g_stack[CH_TIMINGS_STACK_MAX];
static size_t g_depth;
static ChTimingsCacheOutcome g_cache_outcome;
static int g_cache_written;
static char g_cache_path[CH_TIMINGS_PATH_MAX];
static char g_cache_reason[128];
static char g_output_path[CH_TIMINGS_PATH_MAX];

static uint64_t now_ns(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static const char *stage_name(ChTimingsStage stage) {
    switch (stage) {
    case CH_TIMINGS_READ:
        return "read";
    case CH_TIMINGS_EXPAND:
        return "expand";
    case CH_TIMINGS_LOWER:
        return "lower";
    case CH_TIMINGS_OPTIMIZE:
        return "optimize";
    case CH_TIMINGS_EMIT:
        return "emit";
    case CH_TIMINGS_LLVM_EMIT:
        return "llvm_emit";
    case CH_TIMINGS_LINK:
        return "link";
    case CH_TIMINGS_EXECUTE:
        return "execute";
    default:
        return "unknown";
    }
}

static const char *mode_name(ChTimingsMode mode) {
    switch (mode) {
    case CH_TIMINGS_MODE_COMPILE:
        return "compile";
    case CH_TIMINGS_MODE_NATIVE:
        return "native";
    default:
        return "run";
    }
}

void ch_timings_enable(ChTimingsFormat fmt) {
    g_enabled = 1;
    g_format = fmt;
    g_mode = CH_TIMINGS_MODE_RUN;
    memset(g_buckets, 0, sizeof(g_buckets));
    g_depth = 0;
    g_cache_outcome = CH_TIMINGS_CACHE_NONE;
    g_cache_written = 0;
    g_cache_path[0] = '\0';
    g_cache_reason[0] = '\0';
    g_output_path[0] = '\0';
}

int ch_timings_enabled(void) {
    return g_enabled;
}

void ch_timings_set_mode(ChTimingsMode mode) {
    g_mode = mode;
}

void ch_timings_begin(ChTimingsStage stage) {
    if (!g_enabled) {
        return;
    }
    uint64_t t = now_ns();
    if (g_depth > 0 && g_depth <= CH_TIMINGS_STACK_MAX) {
        ChTimingsFrame *top = &g_stack[g_depth - 1];
        if (t >= top->resumed_ns) {
            g_buckets[top->stage] += t - top->resumed_ns;
        }
    }
    if (g_depth < CH_TIMINGS_STACK_MAX) {
        g_stack[g_depth].stage = stage;
        g_stack[g_depth].resumed_ns = t;
    }
    g_depth++;
}

void ch_timings_end(ChTimingsStage stage) {
    if (!g_enabled || g_depth == 0) {
        return;
    }
    uint64_t t = now_ns();
    g_depth--;
    if (g_depth < CH_TIMINGS_STACK_MAX) {
        ChTimingsFrame *frame = &g_stack[g_depth];
        if (frame->stage == stage && t >= frame->resumed_ns) {
            g_buckets[stage] += t - frame->resumed_ns;
        }
        if (g_depth > 0) {
            g_stack[g_depth - 1].resumed_ns = t;
        }
    }
}

void ch_timings_set_cache(ChTimingsCacheOutcome outcome, const char *path, int written,
                          const char *reason) {
    if (!g_enabled) {
        return;
    }
    g_cache_outcome = outcome;
    g_cache_written = written;
    if (path) {
        snprintf(g_cache_path, sizeof(g_cache_path), "%s", path);
    } else {
        g_cache_path[0] = '\0';
    }
    if (reason) {
        snprintf(g_cache_reason, sizeof(g_cache_reason), "%s", reason);
    } else {
        g_cache_reason[0] = '\0';
    }
}

void ch_timings_set_output(const char *path) {
    if (!g_enabled || !path) {
        return;
    }
    snprintf(g_output_path, sizeof(g_output_path), "%s", path);
}

static double ms(uint64_t ns) {
    return (double)ns / 1000000.0;
}

void ch_timings_report(FILE *out) {
    if (!g_enabled || !out) {
        return;
    }
    /* Drain any open frames. */
    while (g_depth > 0) {
        size_t idx = g_depth - 1;
        ChTimingsStage stage =
            idx < CH_TIMINGS_STACK_MAX ? g_stack[idx].stage : CH_TIMINGS_EXECUTE;
        ch_timings_end(stage);
    }

    if (g_format == CH_TIMINGS_JSON) {
        fprintf(out, "{\"mode\":\"%s\",\"stages_ms\":{", mode_name(g_mode));
        int first = 1;
        for (int i = 0; i < CH_TIMINGS_STAGE_COUNT; i++) {
            if (g_buckets[i] == 0) {
                continue;
            }
            if (!first) {
                fputc(',', out);
            }
            first = 0;
            fprintf(out, "\"%s\":%.3f", stage_name((ChTimingsStage)i), ms(g_buckets[i]));
        }
        fprintf(out, "},\"cache\":{");
        const char *status = "none";
        if (g_cache_outcome == CH_TIMINGS_CACHE_HIT) {
            status = "hit";
        } else if (g_cache_outcome == CH_TIMINGS_CACHE_MISS) {
            status = "miss";
        } else if (g_cache_outcome == CH_TIMINGS_CACHE_OFF) {
            status = "off";
        }
        fprintf(out, "\"status\":\"%s\"", status);
        if (g_cache_path[0]) {
            fprintf(out, ",\"path\":\"%s\"", g_cache_path);
        }
        if (g_cache_reason[0]) {
            fprintf(out, ",\"reason\":\"%s\"", g_cache_reason);
        }
        fprintf(out, ",\"written\":%s", g_cache_written ? "true" : "false");
        fprintf(out, "}");
        if (g_output_path[0]) {
            fprintf(out, ",\"output\":\"%s\"", g_output_path);
        }
        fprintf(out, "}\n");
        return;
    }

    fprintf(out, "timings:");
    int any = 0;
    for (int i = 0; i < CH_TIMINGS_STAGE_COUNT; i++) {
        if (g_buckets[i] == 0) {
            continue;
        }
        fprintf(out, "%s %s %.3fms", any ? " |" : "", stage_name((ChTimingsStage)i),
                ms(g_buckets[i]));
        any = 1;
    }
    if (!any) {
        fprintf(out, " (no stages recorded)");
    }
    fputc('\n', out);

    if (g_cache_outcome == CH_TIMINGS_CACHE_HIT) {
        if (g_cache_path[0]) {
            fprintf(out, "cache: HIT (%s)\n", g_cache_path);
        } else {
            fprintf(out, "cache: HIT\n");
        }
    } else if (g_cache_outcome == CH_TIMINGS_CACHE_MISS) {
        fprintf(out, "cache: MISS");
        if (g_cache_written) {
            fprintf(out, " (written)");
        }
        if (g_cache_reason[0]) {
            fprintf(out, " [%s]", g_cache_reason);
        }
        fputc('\n', out);
    } else if (g_cache_outcome == CH_TIMINGS_CACHE_OFF) {
        fprintf(out, "cache: OFF");
        if (g_cache_reason[0]) {
            fprintf(out, " [%s]", g_cache_reason);
        }
        fputc('\n', out);
    }
}
