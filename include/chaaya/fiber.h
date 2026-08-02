#ifndef CHAAYA_FIBER_H
#define CHAAYA_FIBER_H

#include "chaaya/gc.h"
#include "chaaya/reactor.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ChVM ChVM;

#define CH_FIBER_READY_MAX 256
#define CH_CHANNEL_WAITER_MAX 64

typedef enum ChFiberState {
    CH_FIBER_READY = 0,
    CH_FIBER_RUNNING = 1,
    CH_FIBER_DONE = 2,
    CH_FIBER_FAILED = 3,
    CH_FIBER_WAITING = 4,
    CH_FIBER_IO_WAITING = 5,
} ChFiberState;

typedef enum ChFiberParkKind {
    CH_FIBER_PARK_NONE = 0,
    CH_FIBER_PARK_RECV = 1,
    CH_FIBER_PARK_SEND = 2,
    CH_FIBER_PARK_SLEEP = 3,
    CH_FIBER_PARK_JOIN = 4,
    CH_FIBER_PARK_IO = 5,
} ChFiberParkKind;

typedef struct ChFiberSnapshot {
    ChValue *registers;
    size_t register_count;
    ChSavedFrame *frames;
    size_t frame_count;
    ChWindRecord *winds;
    size_t wind_count;
    ChExceptionHandler *handlers;
    size_t handler_count;
    ChParameterBinding *parameter_bindings;
    size_t parameter_binding_count;
    ChSavedUpvalue *open_uvs;
    size_t open_uv_count;
    size_t result_slot;
    size_t entry_frames;
    size_t entry_reg_top;
    int valid;
} ChFiberSnapshot;

typedef enum ChOsThreadState {
    CH_OS_THREAD_NONE = 0,    /* cooperative fiber only */
    CH_OS_THREAD_CREATED = 1,
    CH_OS_THREAD_RUNNING = 2,
    CH_OS_THREAD_DONE = 3,
    CH_OS_THREAD_FAILED = 4,
} ChOsThreadState;

typedef struct ChFiber {
    ChObject header;
    uint64_t id;
    uint8_t state;
    uint8_t park_kind;
    uint8_t queued;
    uint8_t os_state; /* ChOsThreadState */
    uint8_t terminated; /* SRFI-18 thread-terminate! was requested */
    ChValue thunk;
    ChValue result;
    ChValue error;
    ChValue waiting_on;
    ChValue park_payload; /* buffered send value while parked */
    ChValue name;
    ChValue specific;
    ChFiberSnapshot snapshot;
    int io_fd;
    uint8_t io_interest; /* 1=read 2=write */
    void *os_join;       /* ChThreadJoinBox* (opaque; owned by thread runtime) */
} ChFiber;

typedef struct ChChannel {
    ChObject header;
    size_t capacity;    /* 0 => unbounded (or rendezvous when rv_mode) */
    size_t storage_cap; /* allocated ring slots */
    size_t count;
    size_t head;
    size_t tail;
    ChValue *items;
    uint8_t closed;
    uint8_t rendezvous; /* capacity was explicitly 0 at make-channel */
    uint8_t reserved[6];
    ChValue recv_waiters[CH_CHANNEL_WAITER_MAX];
    size_t recv_waiter_count;
    ChValue send_waiters[CH_CHANNEL_WAITER_MAX];
    size_t send_waiter_count;
    void *shared; /* ChSharedChannel* when promoted; NULL otherwise */
} ChChannel;

typedef struct ChFiberRuntime {
    ChValue ready[CH_FIBER_READY_MAX];
    size_t ready_head;
    size_t ready_count;
    ChValue current;
    uint64_t next_id;
    ChReactor reactor;
    size_t entry_frames;
    size_t entry_reg_top;
} ChFiberRuntime;

void ch_fiber_runtime_init(ChFiberRuntime *runtime);
void ch_fiber_runtime_deinit(ChFiberRuntime *runtime);
size_t ch_fiber_runtime_root_count(const ChFiberRuntime *runtime);
size_t ch_fiber_runtime_push_roots(ChGC *gc, ChFiberRuntime *runtime);

ChValue ch_gc_make_fiber(ChGC *gc, uint64_t id, ChValue thunk);
ChValue ch_gc_make_channel(ChGC *gc, size_t capacity, int rendezvous);

int ch_fiber_spawn(ChVM *vm, ChValue thunk, ChValue *out_fiber);
int ch_fiber_yield(ChVM *vm);
int ch_fiber_join(ChVM *vm, ChValue fiber, ChValue *out_result);
/* Timed counterpart of ch_fiber_join: timeout_seconds < 0 waits forever; 0 is
 * an immediate check. Sets *timed_out and returns -1 (no vm->error) when the
 * deadline passes before the fiber finishes. */
int ch_fiber_join_timeout(ChVM *vm, ChValue fiber, double timeout_seconds, ChValue *out_result,
                          int *timed_out);
int ch_fiber_sleep(ChVM *vm, double seconds);
int ch_fiber_wait_fd(ChVM *vm, int fd, ChReactorInterest interest);
int ch_channel_send(ChVM *vm, ChValue channel, ChValue value);
int ch_channel_recv(ChVM *vm, ChValue channel, ChValue *out_value);
/* Timeout variants: timeout_seconds < 0 waits forever; 0 is non-blocking.
 * When timed_out is non-NULL, it is set to 1 on timeout and 0 otherwise. */
int ch_channel_send_timeout(ChVM *vm, ChValue channel, ChValue value, double timeout_seconds,
                            int *timed_out);
int ch_channel_recv_timeout(ChVM *vm, ChValue channel, double timeout_seconds, ChValue *out_value,
                            int *timed_out);
int ch_channel_close(ChVM *vm, ChValue channel);
int ch_channel_closed(ChVM *vm, ChValue channel, int *out_closed);

/* Drive the scheduler until idle or error. */
int ch_fiber_drive(ChVM *vm);

/* Called from VM when a native requests fiber park (vm->fiber_parked). */
int ch_fiber_save_snapshot(ChVM *vm, ChFiber *fiber);
int ch_fiber_restore_snapshot(ChVM *vm, ChFiber *fiber, ChValue inject);

#ifdef __cplusplus
}
#endif

#endif /* CHAAYA_FIBER_H */
