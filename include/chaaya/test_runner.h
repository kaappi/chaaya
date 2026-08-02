#ifndef CHAAYA_TEST_RUNNER_H
#define CHAAYA_TEST_RUNNER_H

#include "chaaya/cli.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Environment channel between orchestrator and worker (Kaappi KAAPPI_TEST_*). */
#define CH_TEST_EMIT_ENV "CHAAYA_TEST_EMIT"
#define CH_TEST_SEED_ENV "CHAAYA_TEST_SEED"

/* Worker: emit path if this process was launched under `chaaya test`. */
const char *ch_test_worker_emit_path(void);

/* Worker: install collecting SRFI-64 runner (+ optional seed). Returns 0 on ok. */
int ch_test_install_collector(ChVM *vm);

/* Worker: write one JSON result object to emit_path. Always best-effort. */
void ch_test_emit_result(ChVM *vm, const char *emit_path, const char *file_path,
                         int toplevel_error, const char *err_msg, double duration_ms);

/* Worker entry: install collector, run file, emit JSON, exit 0. */
int ch_test_run_worker_file(ChVM *vm, const char *path, const char *emit_path);

/* Orchestrator: `chaaya test …`. */
int ch_test_run(const char *argv0, ChCliOptions *opts);

#ifdef __cplusplus
}
#endif

#endif /* CHAAYA_TEST_RUNNER_H */
