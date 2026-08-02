#ifndef CHAAYA_PROFILE_H
#define CHAAYA_PROFILE_H

#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

void ch_profile_enable(void);
int ch_profile_enabled(void);
void ch_profile_enter(const char *name);
void ch_profile_leave(const char *name);
void ch_profile_report_text(FILE *out);
int ch_profile_report_json(const char *path);

#ifdef __cplusplus
}
#endif

#endif /* CHAAYA_PROFILE_H */
