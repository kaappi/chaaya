#ifndef CHAAYA_TIMINGS_H
#define CHAAYA_TIMINGS_H

#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum ChTimingsFormat {
    CH_TIMINGS_TEXT = 0,
    CH_TIMINGS_JSON,
} ChTimingsFormat;

typedef enum ChTimingsMode {
    CH_TIMINGS_MODE_RUN = 0,
    CH_TIMINGS_MODE_COMPILE,
    CH_TIMINGS_MODE_NATIVE,
} ChTimingsMode;

typedef enum ChTimingsStage {
    CH_TIMINGS_READ = 0,
    CH_TIMINGS_EXPAND,
    CH_TIMINGS_LOWER,
    CH_TIMINGS_OPTIMIZE,
    CH_TIMINGS_EMIT,
    CH_TIMINGS_LLVM_EMIT,
    CH_TIMINGS_LINK,
    CH_TIMINGS_EXECUTE,
    CH_TIMINGS_STAGE_COUNT,
} ChTimingsStage;

typedef enum ChTimingsCacheOutcome {
    CH_TIMINGS_CACHE_NONE = 0,
    CH_TIMINGS_CACHE_HIT,
    CH_TIMINGS_CACHE_MISS,
    CH_TIMINGS_CACHE_OFF,
} ChTimingsCacheOutcome;

void ch_timings_enable(ChTimingsFormat fmt);
int ch_timings_enabled(void);
void ch_timings_set_mode(ChTimingsMode mode);

void ch_timings_begin(ChTimingsStage stage);
void ch_timings_end(ChTimingsStage stage);

void ch_timings_set_cache(ChTimingsCacheOutcome outcome, const char *path, int written,
                          const char *reason);
void ch_timings_set_output(const char *path);

void ch_timings_report(FILE *out);

#ifdef __cplusplus
}
#endif

#endif /* CHAAYA_TIMINGS_H */
