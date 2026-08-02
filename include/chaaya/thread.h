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
int ch_thread_check_owner(ChVM *vm, ChValue thread, const char *who);

ChValue ch_gc_make_mutex(ChGC *gc, ChValue name);
ChValue ch_gc_make_condvar(ChGC *gc, ChValue name);
int ch_mutex_lock(ChVM *vm, ChValue mutex, double timeout_seconds);
int ch_mutex_unlock(ChVM *vm, ChValue mutex);
int ch_condvar_signal(ChVM *vm, ChValue condvar);
int ch_condvar_broadcast(ChVM *vm, ChValue condvar);
/* Unlock mutex, wait for signal/timeout, re-lock. timeout < 0 => forever. */
int ch_mutex_unlock_wait(ChVM *vm, ChValue mutex, ChValue condvar, double timeout_seconds);

#ifdef __cplusplus
}
#endif

#endif /* CHAAYA_THREAD_H */
