#ifndef CHAAYA_COVERAGE_H
#define CHAAYA_COVERAGE_H

#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

void ch_coverage_enable(void);
int ch_coverage_enabled(void);

/* Record that an exported library procedure was called. */
void ch_coverage_hit(const char *library, const char *name);

/* Register an export so uncalled procedures appear in the report. */
void ch_coverage_register(const char *library, const char *name);

void ch_coverage_report_text(FILE *out);
int ch_coverage_report_xml(const char *path);

#ifdef __cplusplus
}
#endif

#endif /* CHAAYA_COVERAGE_H */
