#ifndef CHAAYA_DOCTOR_H
#define CHAAYA_DOCTOR_H

#include "chaaya/cli.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Run installation / environment checks. Returns CH_EXIT_OK or CH_EXIT_ERROR. */
int ch_doctor_run(const ChCliOptions *opts);

#ifdef __cplusplus
}
#endif

#endif /* CHAAYA_DOCTOR_H */
