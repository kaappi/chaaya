#ifndef CHAAYA_THREAD_H
#define CHAAYA_THREAD_H

#include "chaaya/gc.h"
#include "chaaya/vm.h"

#ifdef __cplusplus
extern "C" {
#endif

void ch_thread_runtime_init(ChVM *vm);
void ch_thread_runtime_deinit(ChVM *vm);

ChValue ch_thread_current(ChVM *vm);
int ch_thread_make(ChVM *vm, ChValue thunk, ChValue name, ChValue *out);
int ch_thread_start(ChVM *vm, ChValue thread);
int ch_thread_join(ChVM *vm, ChValue thread, ChValue *out_result);
/* Timed counterpart of ch_thread_join. timeout_seconds < 0 => forever (same
 * as ch_thread_join); 0 => immediate check. A never-started thread always
 * reports a timeout (without starting it). Sets *timed_out on expiry;
 * vm->error is left empty in that case (the caller decides whether to raise
 * or hand back a timeout-val). */
int ch_thread_join_timeout(ChVM *vm, ChValue thread, double timeout_seconds, int *timed_out,
                           ChValue *out_result);
/* SRFI-18 thread-terminate!. Real OS threads cannot be safely force-killed
 * from C, so this is a cooperative marker: a not-yet-started or waiting
 * cooperative fiber is failed immediately; a running OS thread keeps
 * executing to completion, but ch_thread_join[_timeout] on it will report a
 * terminated-thread-exception instead of its real outcome. */
int ch_thread_terminate(ChVM *vm, ChValue thread);
int ch_thread_check_owner(ChVM *vm, ChValue thread, const char *who);

ChValue ch_gc_make_mutex(ChGC *gc, ChValue name);
ChValue ch_gc_make_condvar(ChGC *gc, ChValue name);
/* owner_override selects who mutex-lock! records as the new owner on success:
 * CH_UNDEFINED means "the caller's own thread" (the common/default case),
 * CH_FALSE means "no owner" (SRFI 18's explicit-#f-thread convention, used to
 * build higher-level sync objects), and any other value is used verbatim. */
int ch_mutex_lock(ChVM *vm, ChValue mutex, double timeout_seconds, ChValue owner_override);
int ch_mutex_unlock(ChVM *vm, ChValue mutex);
int ch_condvar_signal(ChVM *vm, ChValue condvar);
int ch_condvar_broadcast(ChVM *vm, ChValue condvar);
/* Unlock mutex, wait for signal/timeout, re-lock. timeout < 0 => forever. */
int ch_mutex_unlock_wait(ChVM *vm, ChValue mutex, ChValue condvar, double timeout_seconds);

#ifdef __cplusplus
}
#endif

#endif /* CHAAYA_THREAD_H */
