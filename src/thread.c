#include "chaaya/thread.h"

#include "chaaya/fiber.h"
#include "chaaya/gc_deep_copy.h"
#include "chaaya/reactor.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct ChThreadJoinBox {
    ChVM *parent_vm;
    ChValue parent_fiber;
    ChVM *child_vm;
    ChValue result;
    char error[256];
    int failed;
    int finished;
    pthread_t tid;
    int tid_valid;
    pthread_mutex_t mu;
    pthread_cond_t cv;
} ChThreadJoinBox;

static void set_error(ChVM *vm, const char *msg) {
    if (vm) {
        snprintf(vm->error, sizeof(vm->error), "%s", msg);
    }
}

void ch_thread_runtime_init(ChVM *vm) {
    if (!vm) {
        return;
    }
    vm->owns_globals = true;
    vm->parent_vm = NULL;
    if (!ch_is_fiber(vm->current_thread)) {
        ChValue main_th = ch_gc_make_fiber(&vm->gc, 0, CH_FALSE);
        if (ch_is_fiber(main_th)) {
            ChFiber *f = ch_as_fiber(main_th);
            f->os_state = CH_OS_THREAD_RUNNING;
            f->state = CH_FIBER_RUNNING;
            f->name = ch_gc_intern_symbol_cstr(&vm->gc, "main");
            ch_gc_write_barrier(&vm->gc, &f->header, f->name);
        }
        vm->current_thread = main_th;
    }
}

void ch_thread_runtime_deinit(ChVM *vm) {
    if (!vm) {
        return;
    }
    vm->current_thread = CH_NIL;
}

ChValue ch_thread_current(ChVM *vm) {
    if (!vm) {
        return CH_FALSE;
    }
    if (!ch_is_fiber(vm->current_thread)) {
        ch_thread_runtime_init(vm);
    }
    return vm->current_thread;
}

int ch_thread_check_owner(ChVM *vm, ChValue thread, const char *who) {
    if (!ch_is_fiber(thread)) {
        snprintf(vm->error, sizeof(vm->error), "%s: expected thread", who ? who : "thread");
        return -1;
    }
    ChObject *obj = ch_to_object(thread);
    if (obj->owner != vm->gc.id) {
        snprintf(vm->error, sizeof(vm->error),
                 "%s: thread belongs to another OS thread", who ? who : "thread");
        return -1;
    }
    return 0;
}

int ch_thread_make(ChVM *vm, ChValue thunk, ChValue name, ChValue *out) {
#if defined(__wasi__)
    (void)thunk;
    (void)name;
    (void)out;
    set_error(vm, "make-thread: not available on WASI");
    return -1;
#else
    if (!ch_is_procedure(thunk)) {
        set_error(vm, "make-thread: expected procedure");
        return -1;
    }
    if (!vm->fiber_runtime) {
        set_error(vm, "make-thread: fiber runtime unavailable");
        return -1;
    }
    ChValue fiber = ch_gc_make_fiber(&vm->gc, vm->fiber_runtime->next_id++, thunk);
    ChFiber *f = ch_as_fiber(fiber);
    f->state = CH_FIBER_READY;
    f->os_state = CH_OS_THREAD_CREATED;
    f->queued = 0;
    if (name != CH_FALSE && name != CH_UNDEFINED && name != CH_NIL) {
        f->name = name;
        ch_gc_write_barrier(&vm->gc, &f->header, name);
    }
    if (out) {
        *out = fiber;
    }
    return 0;
#endif
}

typedef struct ThreadStartArg {
    ChThreadJoinBox *box;
    ChVM *child_vm;
    ChValue child_thunk;
} ThreadStartArg;

static void child_vm_cleanup(ChVM *child_vm) {
    if (!child_vm) {
        return;
    }
    if (child_vm->fiber_runtime) {
        ch_fiber_runtime_deinit(child_vm->fiber_runtime);
        free(child_vm->fiber_runtime);
        child_vm->fiber_runtime = NULL;
    }
    free(child_vm->regs);
    child_vm->regs = NULL;
    free(child_vm->frames);
    child_vm->frames = NULL;
    child_vm->libraries = NULL; /* shared with parent */
    ch_gc_deinit(&child_vm->gc);
    free(child_vm);
}

static ChVM *prepare_child_vm(ChVM *parent, ChValue thunk, ChValue *out_child_thunk) {
    ChVM *child_vm = (ChVM *)calloc(1, sizeof(ChVM));
    if (!child_vm) {
        set_error(parent, "thread-start!: out of memory");
        return NULL;
    }
    ch_gc_init_for_thread(&child_vm->gc, &parent->gc);
    child_vm->gc.vm = child_vm;
    child_vm->regs = (ChValue *)calloc(CH_VM_MAX_REGS, sizeof(ChValue));
    child_vm->frames = (ChCallFrame *)calloc(CH_VM_MAX_FRAMES, sizeof(ChCallFrame));
    if (!child_vm->regs || !child_vm->frames) {
        set_error(parent, "thread-start!: out of memory");
        child_vm_cleanup(child_vm);
        return NULL;
    }
    child_vm->owns_globals = false;
    child_vm->parent_vm = parent;
    memcpy(child_vm->globals, parent->globals, sizeof(parent->globals));
    child_vm->global_count = parent->global_count;
    child_vm->libraries = parent->libraries;
    child_vm->lib_path_count = parent->lib_path_count;
    for (size_t i = 0; i < parent->lib_path_count; i++) {
        child_vm->lib_paths[i] = parent->lib_paths[i];
    }
    child_vm->fiber_runtime = (ChFiberRuntime *)calloc(1, sizeof(ChFiberRuntime));
    if (child_vm->fiber_runtime) {
        ch_fiber_runtime_init(child_vm->fiber_runtime);
    }
    child_vm->result = CH_VOID;

    /* Deep-copy on the parent thread so channel promotion is visible before
     * the parent races into channel-receive / join. */
    ChValue child_thunk = ch_gc_deep_copy(&child_vm->gc, thunk);
    if (child_thunk == CH_UNDEFINED) {
        if (child_vm->error[0]) {
            snprintf(parent->error, sizeof(parent->error), "%s", child_vm->error);
        } else {
            set_error(parent, "thread-start!: deep-copy failed");
        }
        child_vm_cleanup(child_vm);
        return NULL;
    }
    ChValue self = ch_gc_make_fiber(&child_vm->gc, 1, child_thunk);
    if (ch_is_fiber(self)) {
        ChFiber *sf = ch_as_fiber(self);
        sf->os_state = CH_OS_THREAD_RUNNING;
        sf->state = CH_FIBER_RUNNING;
    }
    child_vm->current_thread = self;
    if (out_child_thunk) {
        *out_child_thunk = child_thunk;
    }
    return child_vm;
}

static void *thread_entry(void *arg) {
    ThreadStartArg *sa = (ThreadStartArg *)arg;
    ChThreadJoinBox *box = sa->box;
    ChVM *child_vm = sa->child_vm;
    ChValue child_thunk = sa->child_thunk;
    free(sa);

    box->child_vm = child_vm;

    ChValue result = CH_VOID;
    ChVMStatus st = ch_vm_apply(child_vm, child_thunk, NULL, 0, &result);

    pthread_mutex_lock(&box->mu);
    if (st != CH_VM_OK) {
        box->failed = 1;
        snprintf(box->error, sizeof(box->error), "%s",
                 child_vm->error[0] ? child_vm->error : "thread failed");
    } else {
        box->result = ch_coerce_single(result);
        box->failed = 0;
    }
    box->finished = 1;
    pthread_cond_broadcast(&box->cv);
    pthread_mutex_unlock(&box->mu);
    return NULL;
}

int ch_thread_start(ChVM *vm, ChValue thread) {
    if (ch_thread_check_owner(vm, thread, "thread-start!") != 0) {
        return -1;
    }
    ChFiber *f = ch_as_fiber(thread);
    if (f->os_state != CH_OS_THREAD_CREATED) {
        set_error(vm, "thread-start!: thread already started");
        return -1;
    }

    ChValue child_thunk = CH_UNDEFINED;
    ChVM *child_vm = prepare_child_vm(vm, f->thunk, &child_thunk);
    if (!child_vm) {
        return -1;
    }

    ChThreadJoinBox *box = (ChThreadJoinBox *)calloc(1, sizeof(ChThreadJoinBox));
    ThreadStartArg *sa = (ThreadStartArg *)calloc(1, sizeof(ThreadStartArg));
    if (!box || !sa) {
        free(box);
        free(sa);
        child_vm_cleanup(child_vm);
        set_error(vm, "thread-start!: out of memory");
        return -1;
    }
    box->parent_vm = vm;
    box->parent_fiber = thread;
    pthread_mutex_init(&box->mu, NULL);
    pthread_cond_init(&box->cv, NULL);
    sa->box = box;
    sa->child_vm = child_vm;
    sa->child_thunk = child_thunk;

    f->os_join = box;
    f->os_state = CH_OS_THREAD_RUNNING;

    if (pthread_create(&box->tid, NULL, thread_entry, sa) != 0) {
        f->os_state = CH_OS_THREAD_CREATED;
        f->os_join = NULL;
        pthread_mutex_destroy(&box->mu);
        pthread_cond_destroy(&box->cv);
        free(sa);
        free(box);
        child_vm_cleanup(child_vm);
        set_error(vm, "thread-start!: pthread_create failed");
        return -1;
    }
    box->tid_valid = 1;
    return 0;
}

int ch_thread_join(ChVM *vm, ChValue thread, ChValue *out_result) {
    if (ch_eq(thread, ch_thread_current(vm))) {
        set_error(vm, "thread-join!: cannot join current thread");
        return -1;
    }
    if (ch_thread_check_owner(vm, thread, "thread-join!") != 0) {
        return -1;
    }
    ChFiber *f = ch_as_fiber(thread);
    if (f->os_state == CH_OS_THREAD_NONE) {
        return ch_fiber_join(vm, thread, out_result);
    }
    if (f->os_state == CH_OS_THREAD_CREATED) {
        set_error(vm, "thread-join!: thread has not been started");
        return -1;
    }
    ChThreadJoinBox *box = (ChThreadJoinBox *)f->os_join;
    if (!box) {
        set_error(vm, "thread-join!: missing join state");
        return -1;
    }

    pthread_mutex_lock(&box->mu);
    while (!box->finished) {
        pthread_mutex_unlock(&box->mu);
        if (vm->fiber_runtime) {
            (void)ch_fiber_drive(vm);
        }
        struct timespec ts;
        timespec_get(&ts, TIME_UTC);
        ts.tv_nsec += 1000000L;
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec += 1;
            ts.tv_nsec -= 1000000000L;
        }
        pthread_mutex_lock(&box->mu);
        if (!box->finished) {
            (void)pthread_cond_timedwait(&box->cv, &box->mu, &ts);
        }
    }
    int failed = box->failed;
    ChValue child_result = box->result;
    char errbuf[256];
    memcpy(errbuf, box->error, sizeof(errbuf));
    ChVM *child_vm = box->child_vm;
    pthread_t tid = box->tid;
    int tid_valid = box->tid_valid;
    pthread_mutex_unlock(&box->mu);

    if (tid_valid) {
        (void)pthread_join(tid, NULL);
        box->tid_valid = 0;
    }

    if (failed) {
        snprintf(vm->error, sizeof(vm->error), "%s", errbuf[0] ? errbuf : "thread failed");
        f->os_state = CH_OS_THREAD_FAILED;
        if (child_vm) {
            child_vm_cleanup(child_vm);
            box->child_vm = NULL;
        }
        return -1;
    }

    ChValue copied = CH_VOID;
    if (child_vm) {
        copied = ch_gc_deep_copy(&vm->gc, child_result);
        if (copied == CH_UNDEFINED) {
            if (!vm->error[0]) {
                set_error(vm, "thread-join!: result deep-copy failed");
            }
            child_vm_cleanup(child_vm);
            box->child_vm = NULL;
            return -1;
        }
        child_vm_cleanup(child_vm);
        box->child_vm = NULL;
    }

    f->result = copied;
    f->os_state = CH_OS_THREAD_DONE;
    ch_gc_write_barrier(&vm->gc, &f->header, copied);
    if (out_result) {
        *out_result = copied;
    }
    return 0;
}

ChValue ch_gc_make_mutex(ChGC *gc, ChValue name) {
    ch_gc_push(gc, &name);
    ChMutex *m = (ChMutex *)ch_gc_alloc(gc, sizeof(ChMutex), CH_TAG_MUTEX);
    ch_gc_pop(gc);
    m->locked = 0;
    m->abandoned = 0;
    m->owner = CH_NIL;
    m->name = name;
    m->specific = CH_FALSE;
    return ch_make_pointer(&m->header);
}

ChValue ch_gc_make_condvar(ChGC *gc, ChValue name) {
    ch_gc_push(gc, &name);
    ChCondvar *c = (ChCondvar *)ch_gc_alloc(gc, sizeof(ChCondvar), CH_TAG_CONDVAR);
    ch_gc_pop(gc);
    c->signal_generation = 0;
    c->name = name;
    return ch_make_pointer(&c->header);
}

int ch_mutex_lock(ChVM *vm, ChValue mutex, double timeout_seconds) {
    if (!ch_is_mutex(mutex)) {
        set_error(vm, "mutex-lock!: expected mutex");
        return -1;
    }
    ChMutex *m = ch_as_mutex(mutex);
    if (m->header.owner != vm->gc.id) {
        set_error(vm, "mutex-lock!: mutex belongs to another OS thread");
        return -1;
    }
    uint64_t start = ch_reactor_now_ms();
    uint64_t deadline = timeout_seconds < 0
                            ? UINT64_MAX
                            : start + (uint64_t)(timeout_seconds * 1000.0 + 0.5);
    ChValue self = ch_thread_current(vm);
    for (;;) {
        if (!m->locked || m->abandoned) {
            m->locked = 1;
            m->abandoned = 0;
            m->owner = self;
            ch_gc_write_barrier(&vm->gc, &m->header, self);
            return 1;
        }
        if (timeout_seconds == 0.0) {
            return 0;
        }
        if (timeout_seconds > 0.0 && ch_reactor_now_ms() >= deadline) {
            return 0;
        }
        /* Cross-thread / contended: short sleep so siblings and OS threads progress. */
        if (vm->fiber_runtime && ch_is_fiber(vm->fiber_runtime->current)) {
            if (ch_fiber_sleep(vm, 0.001) != 0) {
                return -1;
            }
            if (vm->fiber_parked) {
                return 1; /* parked; resume will re-enter — treat as in-progress */
            }
        } else {
            struct timespec req = {.tv_sec = 0, .tv_nsec = 1000000L};
            (void)nanosleep(&req, NULL);
        }
    }
}

int ch_mutex_unlock(ChVM *vm, ChValue mutex) {
    if (!ch_is_mutex(mutex)) {
        set_error(vm, "mutex-unlock!: expected mutex");
        return -1;
    }
    ChMutex *m = ch_as_mutex(mutex);
    if (m->header.owner != vm->gc.id) {
        set_error(vm, "mutex-unlock!: mutex belongs to another OS thread");
        return -1;
    }
    if (!m->locked) {
        set_error(vm, "mutex-unlock!: mutex is not locked");
        return -1;
    }
    m->locked = 0;
    m->owner = CH_NIL;
    return 0;
}

int ch_condvar_signal(ChVM *vm, ChValue condvar) {
    if (!ch_is_condvar(condvar)) {
        set_error(vm, "condition-variable-signal!: expected condition variable");
        return -1;
    }
    ChCondvar *c = ch_as_condvar(condvar);
    if (c->header.owner != vm->gc.id) {
        set_error(vm, "condition-variable-signal!: belongs to another OS thread");
        return -1;
    }
    c->signal_generation++;
    return 0;
}

int ch_condvar_broadcast(ChVM *vm, ChValue condvar) {
    return ch_condvar_signal(vm, condvar);
}

int ch_mutex_unlock_wait(ChVM *vm, ChValue mutex, ChValue condvar, double timeout_seconds) {
    if (!ch_is_condvar(condvar)) {
        set_error(vm, "mutex-unlock!: expected condition variable");
        return -1;
    }
    ChCondvar *c = ch_as_condvar(condvar);
    uint64_t gen = c->signal_generation;
    if (ch_mutex_unlock(vm, mutex) != 0) {
        return -1;
    }
    uint64_t start = ch_reactor_now_ms();
    uint64_t deadline = timeout_seconds < 0
                            ? UINT64_MAX
                            : start + (uint64_t)(timeout_seconds * 1000.0 + 0.5);
    while (c->signal_generation == gen) {
        if (timeout_seconds >= 0 && ch_reactor_now_ms() >= deadline) {
            break;
        }
        struct timespec req = {.tv_sec = 0, .tv_nsec = 1000000L};
        (void)nanosleep(&req, NULL);
        if (vm->fiber_runtime) {
            (void)ch_fiber_drive(vm);
        }
    }
    if (ch_mutex_lock(vm, mutex, -1.0) != 1) {
        return -1;
    }
    return 0;
}
