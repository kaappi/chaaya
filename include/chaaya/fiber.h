#ifndef CHAAYA_FIBER_H
#define CHAAYA_FIBER_H

#include "chaaya/gc.h"
#include "chaaya/reactor.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ChVM ChVM;

#define CH_FIBER_READY_MAX 256

typedef enum ChFiberState {
    CH_FIBER_READY = 0,
    CH_FIBER_RUNNING = 1,
    CH_FIBER_DONE = 2,
    CH_FIBER_FAILED = 3,
} ChFiberState;

typedef struct ChFiber {
    ChObject header;
    uint64_t id;
    uint8_t state;
    uint8_t reserved[7];
    ChValue thunk;
    ChValue result;
    ChValue error;
} ChFiber;

typedef struct ChChannel {
    ChObject header;
    size_t capacity;    /* 0 => unbounded */
    size_t storage_cap; /* allocated ring slots */
    size_t count;
    size_t head;
    size_t tail;
    ChValue *items;
} ChChannel;

typedef struct ChFiberRuntime {
    ChValue ready[CH_FIBER_READY_MAX];
    size_t ready_head;
    size_t ready_count;
    ChValue current;
    uint64_t next_id;
    ChReactor reactor;
} ChFiberRuntime;

void ch_fiber_runtime_init(ChFiberRuntime *runtime);
void ch_fiber_runtime_deinit(ChFiberRuntime *runtime);
size_t ch_fiber_runtime_root_count(const ChFiberRuntime *runtime);
size_t ch_fiber_runtime_push_roots(ChGC *gc, ChFiberRuntime *runtime);

ChValue ch_gc_make_fiber(ChGC *gc, uint64_t id, ChValue thunk);
ChValue ch_gc_make_channel(ChGC *gc, size_t capacity);

int ch_fiber_spawn(ChVM *vm, ChValue thunk, ChValue *out_fiber);
int ch_fiber_yield(ChVM *vm);
int ch_fiber_join(ChVM *vm, ChValue fiber, ChValue *out_result);
int ch_channel_send(ChVM *vm, ChValue channel, ChValue value);
int ch_channel_recv(ChVM *vm, ChValue channel, ChValue *out_value);

#ifdef __cplusplus
}
#endif

#endif /* CHAAYA_FIBER_H */
