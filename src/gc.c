#include "chaaya/ffi_callback.h"
#include "chaaya/gc.h"
#include "chaaya/vm.h"
#include "chaaya/library.h"
#include "chaaya/environment.h"
#include "chaaya/ffi.h"
#include "chaaya/fiber.h"
#include "chaaya/shared_channel.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint16_t g_next_gc_id = 1;

void ch_gc_init(ChGC *gc) {
    memset(gc, 0, sizeof(*gc));
    gc->threshold = CH_GC_DEFAULT_THRESHOLD;
    gc->promotion_age = CH_GC_DEFAULT_PROMOTION_AGE;
    gc->major_interval = CH_GC_DEFAULT_MAJOR_INTERVAL;
    gc->id = g_next_gc_id++;
    if (gc->id == 0) {
        gc->id = g_next_gc_id++;
    }
    gc->owns_symbols = 1;
    gc->symbol_cap = 64;
    gc->symbols = (ChSymbol **)calloc(gc->symbol_cap, sizeof(ChSymbol *));
    gc->roots = (ChValue **)calloc(CH_GC_ROOT_INITIAL, sizeof(ChValue *));
    gc->root_cap = gc->roots ? CH_GC_ROOT_INITIAL : 0;
}

void ch_gc_init_for_thread(ChGC *gc, ChGC *parent) {
    (void)parent;
    ch_gc_init(gc);
}

static void free_object(ChObject *obj) {
    switch ((ChObjectTag)obj->tag) {
    case CH_TAG_VECTOR: {
        ChVector *vec = (ChVector *)obj;
        free(vec->items);
        break;
    }
    case CH_TAG_STRING: {
        ChString *s = (ChString *)obj;
        free(s->data);
        break;
    }
    case CH_TAG_FUNCTION: {
        ChFunction *fn = (ChFunction *)obj;
        free(fn->code);
        free(fn->constants);
        free(fn->uv_is_local);
        free(fn->uv_index);
        break;
    }
    case CH_TAG_CLOSURE: {
        ChClosure *cl = (ChClosure *)obj;
        free(cl->upvalues);
        break;
    }
    case CH_TAG_CONTINUATION: {
        ChContinuation *c = (ChContinuation *)obj;
        free(c->registers);
        free(c->frames);
        free(c->winds);
        free(c->handlers);
        free(c->parameter_bindings);
        free(c->open_uvs);
        break;
    }
    case CH_TAG_VALUES: {
        ChValues *vs = (ChValues *)obj;
        free(vs->items);
        break;
    }
    case CH_TAG_PORT: {
        ChPort *p = (ChPort *)obj;
        if (p->kind == CH_PORT_FILE && p->file && !p->closed) {
            fclose(p->file);
            p->file = NULL;
        }
        if (p->buf) {
            free(p->buf);
        }
        break;
    }
    case CH_TAG_TRANSFORMER:
        break;
    case CH_TAG_RECORD_TYPE:
        break;
    case CH_TAG_RECORD:
        break;
    case CH_TAG_PROMISE:
        break;
    case CH_TAG_BIGNUM: {
        ChBignum *bn = (ChBignum *)obj;
        free(bn->limbs);
        break;
    }
    case CH_TAG_RATIONAL:
        break;
    case CH_TAG_COMPLEX:
        break;
    case CH_TAG_ENVIRONMENT:
        break;
    case CH_TAG_ERROR_OBJ:
        break;
    case CH_TAG_PARAMETER:
        break;
    case CH_TAG_HASHTABLE: {
        ChHashtable *ht = (ChHashtable *)obj;
        free(ht->keys);
        free(ht->vals);
        free(ht->used);
        break;
    }
    case CH_TAG_BYTEVECTOR:
        break;
    case CH_TAG_TIME:
        break;
    case CH_TAG_FIBER:
        break;
    case CH_TAG_CHANNEL: {
        ChChannel *channel = (ChChannel *)obj;
        if (channel->shared) {
            ch_shared_channel_release((ChSharedChannel *)channel->shared);
            channel->shared = NULL;
        }
        free(channel->items);
        break;
    }
    case CH_TAG_FOREIGN_LIBRARY: {
        ChForeignLibrary *lib = (ChForeignLibrary *)obj;
        ch_ffi_finalize_library(lib);
        break;
    }
    case CH_TAG_FOREIGN_PROC:
        break;
    case CH_TAG_RANDOM_SOURCE:
    case CH_TAG_EPHEMERON:
    case CH_TAG_FILE_INFO:
    case CH_TAG_MUTEX:
    case CH_TAG_CONDVAR:
    case CH_TAG_GROUP_INFO:
        break;
    default:
        break;
    }
    free(obj);
}

static void free_object_list(ChObject *obj) {
    while (obj) {
        ChObject *next = obj->next;
        free_object(obj);
        obj = next;
    }
}

void ch_gc_deinit(ChGC *gc) {
    free_object_list(gc->young_objects);
    free_object_list(gc->old_objects);
    if (gc->owns_symbols) {
        free(gc->symbols);
    }
    free(gc->pending_ephemerons);
    free(gc->remembered_set);
    free(gc->extra_roots);
    free(gc->roots);
    memset(gc, 0, sizeof(*gc));
}

int ch_gc_add_extra_root(ChGC *gc, ChValue value) {
    if (!gc) {
        return -1;
    }
    if (gc->extra_root_count >= gc->extra_root_cap) {
        size_t ncap = gc->extra_root_cap ? gc->extra_root_cap * 2 : 16;
        ChValue *nroots = (ChValue *)realloc(gc->extra_roots, ncap * sizeof(ChValue));
        if (!nroots) {
            return -1;
        }
        gc->extra_roots = nroots;
        gc->extra_root_cap = ncap;
    }
    gc->extra_roots[gc->extra_root_count++] = value;
    return 0;
}

void ch_gc_push(ChGC *gc, ChValue *slot) {
    if (gc->root_count >= gc->root_cap) {
        size_t ncap = gc->root_cap ? gc->root_cap * 2 : CH_GC_ROOT_INITIAL;
        if (ncap > CH_GC_ROOT_MAX) {
            ncap = CH_GC_ROOT_MAX;
        }
        if (ncap <= gc->root_cap) {
            abort(); /* hard ceiling — prefer a catchable re-entrancy error upstream */
        }
        ChValue **nroots = (ChValue **)realloc(gc->roots, ncap * sizeof(ChValue *));
        if (!nroots) {
            abort();
        }
        gc->roots = nroots;
        gc->root_cap = ncap;
    }
    gc->roots[gc->root_count++] = slot;
}

void ch_gc_pop(ChGC *gc) {
    if (gc->root_count == 0) {
        abort();
    }
    gc->root_count--;
}

void ch_gc_pop_n(ChGC *gc, size_t n) {
    if (gc->root_count < n) {
        abort();
    }
    gc->root_count -= n;
}

void ch_gc_pop_to(ChGC *gc, size_t target) {
    if (gc->root_count < target) {
        abort();
    }
    gc->root_count = target;
}

static void mark_value(ChValue v);

static ChGC *g_mark_gc;

static void register_pending_ephemeron(ChValue eph) {
    ChGC *gc = g_mark_gc;
    if (!gc) {
        return;
    }
    if (gc->pending_ephem_count >= gc->pending_ephem_cap) {
        size_t ncap = gc->pending_ephem_cap ? gc->pending_ephem_cap * 2 : 32;
        ChValue *next = (ChValue *)realloc(gc->pending_ephemerons, ncap * sizeof(ChValue));
        if (!next) {
            abort();
        }
        gc->pending_ephemerons = next;
        gc->pending_ephem_cap = ncap;
    }
    gc->pending_ephemerons[gc->pending_ephem_count++] = eph;
}

static int weak_reachable(ChValue v) {
    if (!ch_is_pointer(v)) {
        return 1;
    }
    ChObject *obj = ch_to_object(v);
    return obj && obj->marked;
}

static void process_weak_refs(ChGC *gc) {
    for (;;) {
        int progress = 0;
        size_t i = 0;
        while (i < gc->pending_ephem_count) {
            ChEphemeron *eph = ch_as_ephemeron(gc->pending_ephemerons[i]);
            if (weak_reachable(eph->key)) {
                mark_value(eph->key);
                mark_value(eph->value);
                gc->pending_ephemerons[i] =
                    gc->pending_ephemerons[gc->pending_ephem_count - 1];
                gc->pending_ephem_count--;
                progress = 1;
            } else {
                i++;
            }
        }
        if (!progress) {
            break;
        }
    }
    for (size_t j = 0; j < gc->pending_ephem_count; j++) {
        ChEphemeron *eph = ch_as_ephemeron(gc->pending_ephemerons[j]);
        eph->broken = 1;
        eph->key = CH_FALSE;
        eph->value = CH_FALSE;
    }
    gc->pending_ephem_count = 0;
}

static void mark_object_contents(ChObject *obj);

static void mark_object(ChObject *obj) {
    if (!obj || obj->marked) {
        return;
    }
    obj->marked = 1;
    mark_object_contents(obj);
}

/* Trace referents without setting obj->marked (remembered-set / minor GC). */
static void mark_object_contents(ChObject *obj) {
    if (!obj) {
        return;
    }
    switch ((ChObjectTag)obj->tag) {
    case CH_TAG_PAIR: {
        ChPair *p = (ChPair *)obj;
        mark_value(p->car);
        mark_value(p->cdr);
        break;
    }
    case CH_TAG_VECTOR: {
        ChVector *vec = (ChVector *)obj;
        for (size_t i = 0; i < vec->len; i++) {
            mark_value(vec->items[i]);
        }
        break;
    }
    case CH_TAG_FUNCTION: {
        ChFunction *fn = (ChFunction *)obj;
        for (size_t i = 0; i < fn->const_count; i++) {
            mark_value(fn->constants[i]);
        }
        break;
    }
    case CH_TAG_CLOSURE: {
        ChClosure *cl = (ChClosure *)obj;
        mark_object(&cl->fn->header);
        for (uint8_t i = 0; i < cl->fn->num_upvalues; i++) {
            ChUpvalue *uv = cl->upvalues[i];
            if (uv && uv->is_closed) {
                mark_value(uv->closed_value);
            } else if (uv) {
                mark_value(*uv->location);
            }
        }
        break;
    }
    case CH_TAG_CONTINUATION: {
        ChContinuation *c = (ChContinuation *)obj;
        for (size_t i = 0; i < c->register_count; i++) {
            mark_value(c->registers[i]);
        }
        for (size_t i = 0; i < c->frame_count; i++) {
            if (c->frames[i].closure) {
                mark_object(&c->frames[i].closure->header);
            }
        }
        for (size_t i = 0; i < c->wind_count; i++) {
            mark_value(c->winds[i].before);
            mark_value(c->winds[i].after);
        }
        for (size_t i = 0; i < c->handler_count; i++) {
            mark_value(c->handlers[i].handler);
        }
        for (size_t i = 0; i < c->parameter_binding_count; i++) {
            mark_value(c->parameter_bindings[i].parameter);
            mark_value(c->parameter_bindings[i].value);
        }
        for (size_t i = 0; i < c->open_uv_count; i++) {
            ChUpvalue *uv = c->open_uvs[i].uv;
            if (uv && uv->is_closed) {
                mark_value(uv->closed_value);
            }
        }
        break;
    }
    case CH_TAG_VALUES: {
        ChValues *vs = (ChValues *)obj;
        for (size_t i = 0; i < vs->count; i++) {
            mark_value(vs->items[i]);
        }
        break;
    }
    case CH_TAG_PORT:
    case CH_TAG_SYMBOL:
    case CH_TAG_STRING:
    case CH_TAG_NATIVE:
        break;
    case CH_TAG_TRANSFORMER: {
        ChTransformer *tr = (ChTransformer *)obj;
        for (size_t i = 0; i < tr->literal_count; i++) {
            if (tr->literals[i]) {
                mark_object(&tr->literals[i]->header);
            }
        }
        for (size_t i = 0; i < tr->rule_count; i++) {
            mark_value(tr->patterns[i]);
            mark_value(tr->templates[i]);
        }
        for (size_t i = 0; i < tr->capture_count; i++) {
            if (tr->capture_from[i]) {
                mark_object(&tr->capture_from[i]->header);
            }
            if (tr->capture_to[i]) {
                mark_object(&tr->capture_to[i]->header);
            }
        }
        break;
    }
    case CH_TAG_RECORD_TYPE: {
        ChRecordType *rt = (ChRecordType *)obj;
        mark_value(rt->name);
        if (rt->parent) {
            mark_object(&rt->parent->header);
        }
        break;
    }
    case CH_TAG_RECORD: {
        ChRecord *r = (ChRecord *)obj;
        if (r->rtype) {
            mark_object(&r->rtype->header);
        }
        for (uint16_t i = 0; i < r->num_fields; i++) {
            mark_value(r->fields[i]);
        }
        break;
    }
    case CH_TAG_PROMISE: {
        ChPromise *pr = (ChPromise *)obj;
        mark_value(pr->value);
        break;
    }
    case CH_TAG_BIGNUM:
        break;
    case CH_TAG_RATIONAL: {
        ChRational *r = (ChRational *)obj;
        mark_value(r->numerator);
        mark_value(r->denominator);
        break;
    }
    case CH_TAG_COMPLEX:
        break;
    case CH_TAG_ENVIRONMENT: {
        ChEnvironment *env = (ChEnvironment *)obj;
        for (size_t i = 0; i < env->env.count; i++) {
            if (env->env.bindings[i].name) {
                mark_object(&env->env.bindings[i].name->header);
            }
            if (env->env.bindings[i].defined) {
                mark_value(env->env.bindings[i].value);
            }
        }
        break;
    }
    case CH_TAG_ERROR_OBJ: {
        ChErrorObject *err = (ChErrorObject *)obj;
        mark_value(err->message);
        mark_value(err->irritants);
        break;
    }
    case CH_TAG_PARAMETER: {
        ChParameter *param = (ChParameter *)obj;
        mark_value(param->init);
        mark_value(param->converter);
        mark_value(param->value);
        break;
    }
    case CH_TAG_HASHTABLE: {
        ChHashtable *ht = (ChHashtable *)obj;
        for (size_t i = 0; i < ht->cap; i++) {
            if (!ht->used || !ht->used[i] || ht->keys[i] == CH_UNDEFINED) {
                continue;
            }
            mark_value(ht->keys[i]);
            mark_value(ht->vals[i]);
        }
        break;
    }
    case CH_TAG_BYTEVECTOR:
        break;
    case CH_TAG_TIME: {
        ChTime *time = (ChTime *)obj;
        mark_value(time->type_sym);
        break;
    }
    case CH_TAG_FIBER: {
        ChFiber *fiber = (ChFiber *)obj;
        mark_value(fiber->thunk);
        mark_value(fiber->result);
        mark_value(fiber->error);
        mark_value(fiber->waiting_on);
        mark_value(fiber->park_payload);
        mark_value(fiber->name);
        mark_value(fiber->specific);
        if (fiber->snapshot.valid) {
            for (size_t i = 0; i < fiber->snapshot.register_count; i++) {
                mark_value(fiber->snapshot.registers[i]);
            }
            for (size_t i = 0; i < fiber->snapshot.frame_count; i++) {
                if (fiber->snapshot.frames[i].closure) {
                    mark_object(&fiber->snapshot.frames[i].closure->header);
                }
            }
            for (size_t i = 0; i < fiber->snapshot.wind_count; i++) {
                mark_value(fiber->snapshot.winds[i].before);
                mark_value(fiber->snapshot.winds[i].after);
            }
            for (size_t i = 0; i < fiber->snapshot.handler_count; i++) {
                mark_value(fiber->snapshot.handlers[i].handler);
            }
            for (size_t i = 0; i < fiber->snapshot.parameter_binding_count; i++) {
                mark_value(fiber->snapshot.parameter_bindings[i].parameter);
                mark_value(fiber->snapshot.parameter_bindings[i].value);
            }
        }
        break;
    }
    case CH_TAG_CHANNEL: {
        ChChannel *channel = (ChChannel *)obj;
        if (channel->shared) {
            for (size_t i = 0; i < channel->recv_waiter_count; i++) {
                mark_value(channel->recv_waiters[i]);
            }
            for (size_t i = 0; i < channel->send_waiter_count; i++) {
                mark_value(channel->send_waiters[i]);
            }
            break;
        }
        if (channel->items) {
            for (size_t i = 0; i < channel->count; i++) {
                size_t idx = (channel->head + i) % channel->storage_cap;
                mark_value(channel->items[idx]);
            }
        }
        for (size_t i = 0; i < channel->recv_waiter_count; i++) {
            mark_value(channel->recv_waiters[i]);
        }
        for (size_t i = 0; i < channel->send_waiter_count; i++) {
            mark_value(channel->send_waiters[i]);
        }
        break;
    }
    case CH_TAG_FOREIGN_LIBRARY:
        break;
    case CH_TAG_FOREIGN_PROC: {
        ChForeignProcedure *proc = (ChForeignProcedure *)obj;
        mark_value(proc->library);
        mark_value(proc->name);
        break;
    }
    case CH_TAG_RANDOM_SOURCE:
        break;
    case CH_TAG_EPHEMERON:
        register_pending_ephemeron(ch_make_pointer(obj));
        break;
    case CH_TAG_FILE_INFO:
        break;
    case CH_TAG_MUTEX: {
        ChMutex *m = (ChMutex *)obj;
        mark_value(m->owner);
        mark_value(m->name);
        mark_value(m->specific);
        break;
    }
    case CH_TAG_CONDVAR: {
        ChCondvar *c = (ChCondvar *)obj;
        mark_value(c->name);
        mark_value(c->specific);
        break;
    }
    case CH_TAG_GROUP_INFO:
        mark_value(((ChGroupInfo *)obj)->name);
        break;
    }
}

static int is_young_pointer(ChValue v) {
    if (!ch_is_pointer(v)) {
        return 0;
    }
    ChObject *obj = ch_to_object(v);
    return obj && obj->generation == CH_OBJ_GEN_YOUNG;
}

static int references_young(ChObject *obj) {
    if (!obj) {
        return 0;
    }
    switch ((ChObjectTag)obj->tag) {
    case CH_TAG_PAIR: {
        ChPair *p = (ChPair *)obj;
        return is_young_pointer(p->car) || is_young_pointer(p->cdr);
    }
    case CH_TAG_VECTOR: {
        ChVector *vec = (ChVector *)obj;
        for (size_t i = 0; i < vec->len; i++) {
            if (is_young_pointer(vec->items[i])) {
                return 1;
            }
        }
        return 0;
    }
    case CH_TAG_FUNCTION: {
        ChFunction *fn = (ChFunction *)obj;
        for (size_t i = 0; i < fn->const_count; i++) {
            if (is_young_pointer(fn->constants[i])) {
                return 1;
            }
        }
        return 0;
    }
    case CH_TAG_CLOSURE: {
        ChClosure *cl = (ChClosure *)obj;
        if (!cl->fn) {
            return 0;
        }
        if (cl->fn->header.generation == CH_OBJ_GEN_YOUNG) {
            return 1;
        }
        for (uint8_t i = 0; i < cl->fn->num_upvalues; i++) {
            ChUpvalue *uv = cl->upvalues[i];
            if (!uv) {
                continue;
            }
            if (uv->is_closed) {
                if (is_young_pointer(uv->closed_value)) {
                    return 1;
                }
            } else if (uv->location && is_young_pointer(*uv->location)) {
                return 1;
            }
        }
        return 0;
    }
    case CH_TAG_VALUES: {
        ChValues *vs = (ChValues *)obj;
        for (size_t i = 0; i < vs->count; i++) {
            if (is_young_pointer(vs->items[i])) {
                return 1;
            }
        }
        return 0;
    }
    case CH_TAG_RECORD_TYPE: {
        ChRecordType *rt = (ChRecordType *)obj;
        if (is_young_pointer(rt->name)) {
            return 1;
        }
        return rt->parent && rt->parent->header.generation == CH_OBJ_GEN_YOUNG;
    }
    case CH_TAG_RECORD: {
        ChRecord *r = (ChRecord *)obj;
        if (r->rtype && r->rtype->header.generation == CH_OBJ_GEN_YOUNG) {
            return 1;
        }
        for (uint16_t i = 0; i < r->num_fields; i++) {
            if (is_young_pointer(r->fields[i])) {
                return 1;
            }
        }
        return 0;
    }
    case CH_TAG_PROMISE:
        return is_young_pointer(((ChPromise *)obj)->value);
    case CH_TAG_RATIONAL: {
        ChRational *r = (ChRational *)obj;
        return is_young_pointer(r->numerator) || is_young_pointer(r->denominator);
    }
    case CH_TAG_ENVIRONMENT: {
        ChEnvironment *env = (ChEnvironment *)obj;
        for (size_t i = 0; i < env->env.count; i++) {
            if (env->env.bindings[i].name &&
                env->env.bindings[i].name->header.generation == CH_OBJ_GEN_YOUNG) {
                return 1;
            }
            if (env->env.bindings[i].defined && is_young_pointer(env->env.bindings[i].value)) {
                return 1;
            }
        }
        return 0;
    }
    case CH_TAG_ERROR_OBJ: {
        ChErrorObject *err = (ChErrorObject *)obj;
        return is_young_pointer(err->message) || is_young_pointer(err->irritants);
    }
    case CH_TAG_PARAMETER: {
        ChParameter *param = (ChParameter *)obj;
        return is_young_pointer(param->init) || is_young_pointer(param->converter) ||
               is_young_pointer(param->value);
    }
    case CH_TAG_HASHTABLE: {
        ChHashtable *ht = (ChHashtable *)obj;
        for (size_t i = 0; i < ht->cap; i++) {
            if (!ht->used || !ht->used[i] || ht->keys[i] == CH_UNDEFINED) {
                continue;
            }
            if (is_young_pointer(ht->keys[i]) || is_young_pointer(ht->vals[i])) {
                return 1;
            }
        }
        return 0;
    }
    case CH_TAG_TIME:
        return is_young_pointer(((ChTime *)obj)->type_sym);
    case CH_TAG_FIBER: {
        ChFiber *fiber = (ChFiber *)obj;
        if (is_young_pointer(fiber->thunk) || is_young_pointer(fiber->result) ||
            is_young_pointer(fiber->error) || is_young_pointer(fiber->waiting_on) ||
            is_young_pointer(fiber->park_payload) || is_young_pointer(fiber->name) ||
            is_young_pointer(fiber->specific)) {
            return 1;
        }
        if (fiber->snapshot.valid) {
            for (size_t i = 0; i < fiber->snapshot.register_count; i++) {
                if (is_young_pointer(fiber->snapshot.registers[i])) {
                    return 1;
                }
            }
        }
        return 0;
    }
    case CH_TAG_CHANNEL: {
        ChChannel *channel = (ChChannel *)obj;
        if (channel->shared) {
            for (size_t i = 0; i < channel->recv_waiter_count; i++) {
                if (is_young_pointer(channel->recv_waiters[i])) {
                    return 1;
                }
            }
            for (size_t i = 0; i < channel->send_waiter_count; i++) {
                if (is_young_pointer(channel->send_waiters[i])) {
                    return 1;
                }
            }
            return 0;
        }
        if (channel->items) {
            for (size_t i = 0; i < channel->count; i++) {
                size_t idx = (channel->head + i) % channel->storage_cap;
                if (is_young_pointer(channel->items[idx])) {
                    return 1;
                }
            }
        }
        for (size_t i = 0; i < channel->recv_waiter_count; i++) {
            if (is_young_pointer(channel->recv_waiters[i])) {
                return 1;
            }
        }
        for (size_t i = 0; i < channel->send_waiter_count; i++) {
            if (is_young_pointer(channel->send_waiters[i])) {
                return 1;
            }
        }
        return 0;
    }
    case CH_TAG_FOREIGN_PROC: {
        ChForeignProcedure *proc = (ChForeignProcedure *)obj;
        return is_young_pointer(proc->library) || is_young_pointer(proc->name);
    }
    case CH_TAG_TRANSFORMER: {
        ChTransformer *tr = (ChTransformer *)obj;
        for (size_t i = 0; i < tr->literal_count; i++) {
            if (tr->literals[i] && tr->literals[i]->header.generation == CH_OBJ_GEN_YOUNG) {
                return 1;
            }
        }
        for (size_t i = 0; i < tr->rule_count; i++) {
            if (is_young_pointer(tr->patterns[i]) || is_young_pointer(tr->templates[i])) {
                return 1;
            }
        }
        for (size_t i = 0; i < tr->capture_count; i++) {
            if ((tr->capture_from[i] &&
                 tr->capture_from[i]->header.generation == CH_OBJ_GEN_YOUNG) ||
                (tr->capture_to[i] && tr->capture_to[i]->header.generation == CH_OBJ_GEN_YOUNG)) {
                return 1;
            }
        }
        return 0;
    }
    case CH_TAG_CONTINUATION: {
        ChContinuation *c = (ChContinuation *)obj;
        for (size_t i = 0; i < c->register_count; i++) {
            if (is_young_pointer(c->registers[i])) {
                return 1;
            }
        }
        for (size_t i = 0; i < c->frame_count; i++) {
            if (c->frames[i].closure &&
                c->frames[i].closure->header.generation == CH_OBJ_GEN_YOUNG) {
                return 1;
            }
        }
        for (size_t i = 0; i < c->wind_count; i++) {
            if (is_young_pointer(c->winds[i].before) || is_young_pointer(c->winds[i].after)) {
                return 1;
            }
        }
        for (size_t i = 0; i < c->handler_count; i++) {
            if (is_young_pointer(c->handlers[i].handler)) {
                return 1;
            }
        }
        for (size_t i = 0; i < c->parameter_binding_count; i++) {
            if (is_young_pointer(c->parameter_bindings[i].parameter) ||
                is_young_pointer(c->parameter_bindings[i].value)) {
                return 1;
            }
        }
        return 0;
    }
    case CH_TAG_MUTEX: {
        ChMutex *m = (ChMutex *)obj;
        return is_young_pointer(m->owner) || is_young_pointer(m->name) ||
               is_young_pointer(m->specific);
    }
    case CH_TAG_CONDVAR: {
        ChCondvar *c = (ChCondvar *)obj;
        return is_young_pointer(c->name) || is_young_pointer(c->specific);
    }
    case CH_TAG_GROUP_INFO:
        return is_young_pointer(((ChGroupInfo *)obj)->name);
    default:
        return 0;
    }
}

static void remembered_set_add(ChGC *gc, ChObject *container) {
    if (!gc || !container || !ch_object_is_old(container)) {
        return;
    }
    if (gc->remembered_count >= gc->remembered_cap) {
        size_t ncap = gc->remembered_cap ? gc->remembered_cap * 2 : 64;
        ChObject **next =
            (ChObject **)realloc(gc->remembered_set, ncap * sizeof(ChObject *));
        if (!next) {
            abort();
        }
        gc->remembered_set = next;
        gc->remembered_cap = ncap;
    }
    gc->remembered_set[gc->remembered_count++] = container;
}

static void prune_remembered_set(ChGC *gc) {
    size_t write = 0;
    for (size_t i = 0; i < gc->remembered_count; i++) {
        ChObject *obj = gc->remembered_set[i];
        if (obj && ch_object_is_old(obj) && references_young(obj)) {
            gc->remembered_set[write++] = obj;
        }
    }
    gc->remembered_count = write;
}

static void mark_value(ChValue v) {
    if (ch_is_pointer(v)) {
        mark_object(ch_to_object(v));
    }
}

void ch_gc_mark_value(ChValue v) {
    mark_value(v);
}

static void mark_roots_and_symbols(ChGC *gc) {
    gc->pending_ephem_count = 0;
    g_mark_gc = gc;
    if (gc->vm) {
        ch_vm_mark_gc_roots(gc->vm);
        ch_library_mark_gc_roots(gc->vm);
        ch_ffi_callback_mark_roots();
    }
    for (size_t i = 0; i < gc->compiling_fn_depth; i++) {
        if (gc->compiling_fns[i]) {
            mark_object(&gc->compiling_fns[i]->header);
        }
    }
    for (size_t i = 0; i < gc->root_count; i++) {
        mark_value(*gc->roots[i]);
    }
    for (size_t i = 0; i < gc->extra_root_count; i++) {
        mark_value(gc->extra_roots[i]);
    }
    for (size_t i = 0; i < gc->symbol_count; i++) {
        if (gc->symbols[i]) {
            mark_object(&gc->symbols[i]->header);
        }
    }
    /* Caller finishes weak fixpoint after any remembered-set tracing. */
}

static void sweep_list_major(ChGC *gc, ChObject **head, size_t *count) {
    ChObject **link = head;
    while (*link) {
        ChObject *obj = *link;
        if (!obj->marked) {
            *link = obj->next;
            free_object(obj);
            (*count)--;
            gc->object_count--;
        } else {
            obj->marked = 0;
            link = &obj->next;
        }
    }
}

static void clear_marks(ChObject *list) {
    for (ChObject *obj = list; obj; obj = obj->next) {
        obj->marked = 0;
    }
}

static void sweep_young_minor(ChGC *gc) {
    ChObject **link = &gc->young_objects;
    while (*link) {
        ChObject *obj = *link;
        if (!obj->marked) {
            *link = obj->next;
            free_object(obj);
            gc->young_count--;
            gc->object_count--;
            continue;
        }

        obj->marked = 0;
        obj->age = (uint8_t)(obj->age + 1);
        if (obj->age >= gc->promotion_age) {
            *link = obj->next;
            gc->young_count--;
            obj->generation = CH_OBJ_GEN_OLD;
            obj->age = 0;
            obj->next = gc->old_objects;
            gc->old_objects = obj;
            gc->old_count++;
            /* Newly old container may still point at young siblings. */
            if (references_young(obj)) {
                remembered_set_add(gc, obj);
            }
            continue;
        }
        link = &obj->next;
    }
}

void ch_gc_collect_minor(ChGC *gc) {
    clear_marks(gc->old_objects);
    mark_roots_and_symbols(gc);
    for (size_t i = 0; i < gc->remembered_count; i++) {
        mark_object_contents(gc->remembered_set[i]);
    }
    process_weak_refs(gc);
    g_mark_gc = NULL;
    sweep_young_minor(gc);
    prune_remembered_set(gc);
    clear_marks(gc->old_objects);
    gc->alloc_count = 0;
    gc->minor_collections++;
    gc->collections++;
    gc->threshold = gc->object_count * 2 + CH_GC_DEFAULT_THRESHOLD;
}

void ch_gc_collect_major(ChGC *gc) {
    mark_roots_and_symbols(gc);
    process_weak_refs(gc);
    g_mark_gc = NULL;
    sweep_list_major(gc, &gc->young_objects, &gc->young_count);
    sweep_list_major(gc, &gc->old_objects, &gc->old_count);
    gc->remembered_count = 0;
    gc->alloc_count = 0;
    gc->major_collections++;
    gc->collections++;
    gc->threshold = gc->object_count * 2 + CH_GC_DEFAULT_THRESHOLD;
}

void ch_gc_collect(ChGC *gc) {
    ch_gc_collect_major(gc);
}

void ch_gc_write_barrier(ChGC *gc, ChObject *owner, ChValue value) {
    if (!gc || !owner || !ch_object_is_old(owner) || !ch_is_pointer(value)) {
        return;
    }
    ChObject *child = ch_to_object(value);
    if (!child || child->generation != CH_OBJ_GEN_YOUNG) {
        return;
    }
    remembered_set_add(gc, owner);
}

void ch_gc_promote_to_old(ChGC *gc, ChObject *obj) {
    if (!gc || !obj || obj->generation != CH_OBJ_GEN_YOUNG) {
        return;
    }
    ChObject **link = &gc->young_objects;
    while (*link && *link != obj) {
        link = &(*link)->next;
    }
    if (*link != obj) {
        return;
    }
    *link = obj->next;
    if (gc->young_count > 0) {
        gc->young_count--;
    }
    obj->generation = CH_OBJ_GEN_OLD;
    obj->age = 0;
    obj->next = gc->old_objects;
    gc->old_objects = obj;
    gc->old_count++;
    if (references_young(obj)) {
        remembered_set_add(gc, obj);
    }
}

void *ch_gc_alloc(ChGC *gc, size_t size, ChObjectTag tag) {
    if (gc->no_collect == 0 && gc->alloc_count >= gc->threshold) {
        ch_gc_collect_minor(gc);
        if (gc->major_interval != 0 &&
            (gc->minor_collections % (size_t)gc->major_interval) == 0) {
            ch_gc_collect_major(gc);
        }
    }
    ChObject *obj = (ChObject *)calloc(1, size);
    if (!obj) {
        abort();
    }
    obj->tag = (uint8_t)tag;
    obj->marked = 0;
    obj->generation = CH_OBJ_GEN_YOUNG;
    obj->age = 0;
    obj->owner = gc->id;
    obj->next = gc->young_objects;
    gc->young_objects = obj;
    gc->young_count++;
    gc->object_count++;
    gc->alloc_count++;
    return obj;
}

ChValue ch_gc_cons(ChGC *gc, ChValue car, ChValue cdr) {
    ChValue car_r = car;
    ChValue cdr_r = cdr;
    ch_gc_push(gc, &car_r);
    ch_gc_push(gc, &cdr_r);
    ChPair *p = (ChPair *)ch_gc_alloc(gc, sizeof(ChPair), CH_TAG_PAIR);
    p->car = car_r;
    p->cdr = cdr_r;
    ch_gc_pop_n(gc, 2);
    return ch_make_pointer(&p->header);
}

ChValue ch_gc_make_string(ChGC *gc, const char *bytes, size_t len) {
    ChString *s = (ChString *)ch_gc_alloc(gc, sizeof(ChString), CH_TAG_STRING);
    char *buf = (char *)malloc(len + 1);
    if (!buf) {
        abort();
    }
    if (len > 0) {
        memcpy(buf, bytes, len);
    }
    buf[len] = '\0';
    s->len = len;
    s->data = buf;
    return ch_make_pointer(&s->header);
}

ChValue ch_gc_make_string_cstr(ChGC *gc, const char *cstr) {
    return ch_gc_make_string(gc, cstr, strlen(cstr));
}

ChValue ch_gc_intern_symbol(ChGC *gc, const char *name, size_t len) {
    for (size_t i = 0; i < gc->symbol_count; i++) {
        ChSymbol *sym = gc->symbols[i];
        if (sym->len == len && memcmp(sym->name, name, len) == 0) {
            return ch_make_pointer(&sym->header);
        }
    }
    if (gc->symbol_count >= gc->symbol_cap) {
        size_t ncap = gc->symbol_cap * 2;
        ChSymbol **ns = (ChSymbol **)realloc(gc->symbols, ncap * sizeof(ChSymbol *));
        if (!ns) {
            abort();
        }
        gc->symbols = ns;
        gc->symbol_cap = ncap;
    }
    ChSymbol *sym = (ChSymbol *)ch_gc_alloc(gc, sizeof(ChSymbol) + len + 1, CH_TAG_SYMBOL);
    sym->len = len;
    memcpy(sym->name, name, len);
    sym->name[len] = '\0';
    gc->symbols[gc->symbol_count++] = sym;
    return ch_make_pointer(&sym->header);
}

ChValue ch_gc_intern_symbol_cstr(ChGC *gc, const char *name) {
    return ch_gc_intern_symbol(gc, name, strlen(name));
}

ChValue ch_gc_alloc_uninterned_symbol(ChGC *gc, const char *name, size_t len) {
    ChSymbol *sym = (ChSymbol *)ch_gc_alloc(gc, sizeof(ChSymbol) + len + 1, CH_TAG_SYMBOL);
    sym->len = len;
    memcpy(sym->name, name, len);
    sym->name[len] = '\0';
    ch_object_set_immutable(&sym->header, true);
    sym->header.reserved = (uint16_t)(sym->header.reserved | CH_OBJ_FLAG_UNINTERNED);
    return ch_make_pointer(&sym->header);
}

ChValue ch_gc_alloc_uninterned_symbol_cstr(ChGC *gc, const char *name) {
    return ch_gc_alloc_uninterned_symbol(gc, name, strlen(name));
}

ChValue ch_gc_make_vector(ChGC *gc, size_t len, ChValue fill) {
    ChValue fill_r = fill;
    ch_gc_push(gc, &fill_r);
    ChVector *vec = (ChVector *)ch_gc_alloc(gc, sizeof(ChVector), CH_TAG_VECTOR);
    vec->len = len;
    vec->items = (ChValue *)calloc(len == 0 ? 1 : len, sizeof(ChValue));
    if (!vec->items) {
        abort();
    }
    for (size_t i = 0; i < len; i++) {
        vec->items[i] = fill_r;
    }
    ch_gc_pop(gc);
    return ch_make_pointer(&vec->header);
}

ChValue ch_gc_make_function(ChGC *gc) {
    ChFunction *fn = (ChFunction *)ch_gc_alloc(gc, sizeof(ChFunction), CH_TAG_FUNCTION);
    return ch_make_pointer(&fn->header);
}

ChValue ch_gc_make_closure(ChGC *gc, ChFunction *fn, ChUpvalue **upvalues) {
    ChValue fn_v = ch_make_pointer(&fn->header);
    ch_gc_push(gc, &fn_v);
    ChClosure *cl = (ChClosure *)ch_gc_alloc(gc, sizeof(ChClosure), CH_TAG_CLOSURE);
    cl->fn = (ChFunction *)ch_to_object(fn_v);
    cl->upvalues = upvalues;
    cl->home_env = NULL;
    ch_gc_pop(gc);
    return ch_make_pointer(&cl->header);
}

ChValue ch_gc_make_native(ChGC *gc, ChNativeFn fn, const char *name, int arity, int min_arity) {
    ChNative *n = (ChNative *)ch_gc_alloc(gc, sizeof(ChNative), CH_TAG_NATIVE);
    n->fn = fn;
    n->name = name;
    n->arity = arity;
    n->min_arity = min_arity;
    return ch_make_pointer(&n->header);
}

ChValue ch_gc_make_continuation(ChGC *gc) {
    ChContinuation *c = (ChContinuation *)ch_gc_alloc(gc, sizeof(ChContinuation), CH_TAG_CONTINUATION);
    return ch_make_pointer(&c->header);
}

ChValue ch_gc_make_values(ChGC *gc, ChValue *items, size_t count) {
    ChValues *vs = (ChValues *)ch_gc_alloc(gc, sizeof(ChValues), CH_TAG_VALUES);
    vs->count = count;
    vs->items = (ChValue *)calloc(count == 0 ? 1 : count, sizeof(ChValue));
    if (!vs->items) {
        abort();
    }
    for (size_t i = 0; i < count; i++) {
        vs->items[i] = items[i];
    }
    return ch_make_pointer(&vs->header);
}

static void init_port_common(ChPort *p, ChPortKind kind, int input, int output) {
    p->kind = kind;
    p->input = (uint8_t)(input ? 1 : 0);
    p->output = (uint8_t)(output ? 1 : 0);
    p->closed = 0;
    p->binary = 0;
    p->nonblocking = 0;
    p->file = NULL;
    p->buf = NULL;
    p->len = 0;
    p->cap = 0;
    p->pos = 0;
}

ChValue ch_gc_make_stdio_port(ChGC *gc, FILE *file, int input, int output) {
    ChPort *p = (ChPort *)ch_gc_alloc(gc, sizeof(ChPort), CH_TAG_PORT);
    init_port_common(p, CH_PORT_STDIO, input, output);
    p->file = file;
    return ch_make_pointer(&p->header);
}

ChValue ch_gc_make_string_input_port(ChGC *gc, const char *bytes, size_t len) {
    ChPort *p = (ChPort *)ch_gc_alloc(gc, sizeof(ChPort), CH_TAG_PORT);
    init_port_common(p, CH_PORT_STRING_IN, 1, 0);
    p->buf = (char *)malloc(len + 1);
    if (!p->buf) {
        abort();
    }
    if (len > 0) {
        memcpy(p->buf, bytes, len);
    }
    p->buf[len] = '\0';
    p->len = len;
    p->cap = len + 1;
    p->pos = 0;
    return ch_make_pointer(&p->header);
}

ChValue ch_gc_make_string_output_port(ChGC *gc) {
    ChPort *p = (ChPort *)ch_gc_alloc(gc, sizeof(ChPort), CH_TAG_PORT);
    init_port_common(p, CH_PORT_STRING_OUT, 0, 1);
    p->cap = 64;
    p->buf = (char *)malloc(p->cap);
    if (!p->buf) {
        abort();
    }
    p->buf[0] = '\0';
    p->len = 0;
    p->pos = 0;
    return ch_make_pointer(&p->header);
}

ChValue ch_gc_make_bytevector_input_port(ChGC *gc, const uint8_t *bytes, size_t len) {
    ChPort *p = (ChPort *)ch_gc_alloc(gc, sizeof(ChPort), CH_TAG_PORT);
    init_port_common(p, CH_PORT_BYTEVECTOR, 1, 0);
    p->cap = len;
    if (len > 0) {
        p->buf = (char *)malloc(len);
        if (!p->buf) {
            abort();
        }
        memcpy(p->buf, bytes, len);
    }
    p->len = len;
    p->pos = 0;
    return ch_make_pointer(&p->header);
}

ChValue ch_gc_make_bytevector_output_port(ChGC *gc) {
    ChPort *p = (ChPort *)ch_gc_alloc(gc, sizeof(ChPort), CH_TAG_PORT);
    init_port_common(p, CH_PORT_BYTEVECTOR, 0, 1);
    p->cap = 64;
    p->buf = (char *)malloc(p->cap);
    if (!p->buf) {
        abort();
    }
    p->len = 0;
    p->pos = 0;
    return ch_make_pointer(&p->header);
}

ChValue ch_gc_make_transformer(ChGC *gc) {
    ChTransformer *tr = (ChTransformer *)ch_gc_alloc(gc, sizeof(ChTransformer), CH_TAG_TRANSFORMER);
    tr->literal_count = 0;
    tr->ellipsis_id = NULL;
    tr->rule_count = 0;
    tr->capture_count = 0;
    return ch_make_pointer(&tr->header);
}

ChValue ch_gc_make_record_type(ChGC *gc, ChValue name, uint16_t num_fields) {
    ch_gc_push(gc, &name);
    ChRecordType *rt = (ChRecordType *)ch_gc_alloc(gc, sizeof(ChRecordType), CH_TAG_RECORD_TYPE);
    ch_gc_pop(gc);
    rt->name = name;
    rt->num_fields = num_fields;
    rt->own_field_start = 0;
    rt->parent = NULL;
    return ch_make_pointer(&rt->header);
}

ChValue ch_gc_make_record_type_ext(ChGC *gc, ChValue name, uint16_t own_fields, ChRecordType *parent) {
    uint16_t parent_n = parent ? parent->num_fields : 0;
    uint32_t total = (uint32_t)parent_n + (uint32_t)own_fields;
    if (total > CH_RECORD_MAX_FIELDS) {
        return CH_UNDEFINED;
    }
    ch_gc_push(gc, &name);
    ChValue parent_v = parent ? ch_make_pointer(&parent->header) : CH_NIL;
    if (parent) {
        ch_gc_push(gc, &parent_v);
    }
    ChRecordType *rt = (ChRecordType *)ch_gc_alloc(gc, sizeof(ChRecordType), CH_TAG_RECORD_TYPE);
    ch_gc_pop_n(gc, parent ? 2 : 1);
    rt->name = name;
    rt->num_fields = (uint16_t)total;
    rt->own_field_start = parent_n;
    rt->parent = parent;
    return ch_make_pointer(&rt->header);
}

ChValue ch_gc_make_record(ChGC *gc, ChRecordType *rtype, ChValue *fields, uint16_t nfields) {
    ChValue rtv = ch_make_pointer(&rtype->header);
    ch_gc_push(gc, &rtv);
    for (uint16_t i = 0; i < nfields; i++) {
        ch_gc_push(gc, &fields[i]);
    }
    size_t bytes = sizeof(ChRecord) + (size_t)nfields * sizeof(ChValue);
    ChRecord *r = (ChRecord *)ch_gc_alloc(gc, bytes, CH_TAG_RECORD);
    ch_gc_pop_n(gc, 1 + (size_t)nfields);
    r->rtype = rtype;
    r->num_fields = nfields;
    for (uint16_t i = 0; i < nfields; i++) {
        r->fields[i] = fields[i];
    }
    return ch_make_pointer(&r->header);
}

ChValue ch_gc_make_promise(ChGC *gc, int forced, ChValue value) {
    ch_gc_push(gc, &value);
    ChPromise *pr = (ChPromise *)ch_gc_alloc(gc, sizeof(ChPromise), CH_TAG_PROMISE);
    ch_gc_pop(gc);
    pr->forced = forced ? 1 : 0;
    pr->forcing = 0;
    pr->value = value;
    return ch_make_pointer(&pr->header);
}

ChValue ch_gc_make_file_port(ChGC *gc, FILE *file, int input, int output, int binary) {
    ChPort *p = (ChPort *)ch_gc_alloc(gc, sizeof(ChPort), CH_TAG_PORT);
    init_port_common(p, CH_PORT_FILE, input, output);
    p->binary = (uint8_t)(binary ? 1 : 0);
    p->file = file;
    return ch_make_pointer(&p->header);
}

ChValue ch_gc_make_error_object(ChGC *gc, ChValue message, ChValue irritants, uint8_t error_type) {
    ch_gc_push(gc, &message);
    ch_gc_push(gc, &irritants);
    ChErrorObject *err = (ChErrorObject *)ch_gc_alloc(gc, sizeof(ChErrorObject), CH_TAG_ERROR_OBJ);
    ch_gc_pop_n(gc, 2);
    err->message = message;
    err->irritants = irritants;
    err->error_type = error_type;
    return ch_make_pointer(&err->header);
}

ChValue ch_gc_make_parameter(ChGC *gc, ChValue init, ChValue converter) {
    ch_gc_push(gc, &init);
    ch_gc_push(gc, &converter);
    ChParameter *param = (ChParameter *)ch_gc_alloc(gc, sizeof(ChParameter), CH_TAG_PARAMETER);
    ch_gc_pop_n(gc, 2);
    param->init = init;
    param->converter = converter;
    param->value = init;
    return ch_make_pointer(&param->header);
}

ChValue ch_gc_make_hashtable(ChGC *gc, size_t capacity) {
    if (capacity == 0) {
        capacity = 8;
    }
    ChHashtable *ht = (ChHashtable *)ch_gc_alloc(gc, sizeof(ChHashtable), CH_TAG_HASHTABLE);
    ht->count = 0;
    ht->cap = capacity;
    ht->mode = CH_HASHTABLE_EQV;
    ht->keys = (ChValue *)calloc(capacity, sizeof(ChValue));
    ht->vals = (ChValue *)calloc(capacity, sizeof(ChValue));
    ht->used = (bool *)calloc(capacity, sizeof(bool));
    if (!ht->keys || !ht->vals || !ht->used) {
        abort();
    }
    for (size_t i = 0; i < capacity; i++) {
        ht->keys[i] = CH_UNDEFINED;
        ht->vals[i] = CH_UNDEFINED;
    }
    return ch_make_pointer(&ht->header);
}

ChValue ch_gc_make_bytevector(ChGC *gc, size_t len, uint8_t fill) {
    ChBytevector *bv =
        (ChBytevector *)ch_gc_alloc(gc, sizeof(ChBytevector) + len, CH_TAG_BYTEVECTOR);
    bv->len = len;
    if (len > 0) {
        memset(bv->data, fill, len);
    }
    return ch_make_pointer(&bv->header);
}

ChValue ch_gc_make_time(ChGC *gc, int64_t seconds, int32_t nanoseconds, ChValue type_sym) {
    ch_gc_push(gc, &type_sym);
    ChTime *time = (ChTime *)ch_gc_alloc(gc, sizeof(ChTime), CH_TAG_TIME);
    ch_gc_pop(gc);
    time->seconds = seconds;
    time->nanoseconds = nanoseconds;
    time->type_sym = type_sym;
    return ch_make_pointer(&time->header);
}

static uint64_t splitmix64_next(uint64_t *state) {
    uint64_t z = (*state += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

static uint64_t rotl64(uint64_t x, int k) {
    return (x << k) | (x >> (64 - k));
}

void ch_random_source_seed(ChRandomSource *rs, uint64_t seed) {
    uint64_t sm = seed;
    for (int i = 0; i < 4; i++) {
        rs->s[i] = splitmix64_next(&sm);
    }
}

ChValue ch_gc_make_random_source(ChGC *gc, uint64_t seed) {
    ChRandomSource *rs =
        (ChRandomSource *)ch_gc_alloc(gc, sizeof(ChRandomSource), CH_TAG_RANDOM_SOURCE);
    ch_random_source_seed(rs, seed);
    return ch_make_pointer(&rs->header);
}

uint64_t ch_random_source_next_u64(ChRandomSource *rs) {
    const uint64_t result = rotl64(rs->s[1] * 5, 7) * 9;
    const uint64_t t = rs->s[1] << 17;
    rs->s[2] ^= rs->s[0];
    rs->s[3] ^= rs->s[1];
    rs->s[1] ^= rs->s[2];
    rs->s[0] ^= rs->s[3];
    rs->s[2] ^= t;
    rs->s[3] = rotl64(rs->s[3], 45);
    return result;
}

ChValue ch_gc_make_ephemeron(ChGC *gc, ChValue key, ChValue value) {
    ChValue key_r = key;
    ChValue val_r = value;
    ch_gc_push(gc, &key_r);
    ch_gc_push(gc, &val_r);
    ChEphemeron *eph = (ChEphemeron *)ch_gc_alloc(gc, sizeof(ChEphemeron), CH_TAG_EPHEMERON);
    eph->key = key_r;
    eph->value = val_r;
    eph->broken = 0;
    ch_gc_pop_n(gc, 2);
    return ch_make_pointer(&eph->header);
}

ChValue ch_gc_make_file_info(ChGC *gc, const ChFileInfo *info) {
    ChFileInfo *fi = (ChFileInfo *)ch_gc_alloc(gc, sizeof(ChFileInfo), CH_TAG_FILE_INFO);
    fi->mode = info->mode;
    fi->size = info->size;
    fi->mtime_sec = info->mtime_sec;
    fi->atime_sec = info->atime_sec;
    fi->ctime_sec = info->ctime_sec;
    fi->dev = info->dev;
    fi->ino = info->ino;
    fi->nlinks = info->nlinks;
    fi->rdev = info->rdev;
    fi->blksize = info->blksize;
    fi->blocks = info->blocks;
    fi->uid = info->uid;
    fi->gid = info->gid;
    return ch_make_pointer(&fi->header);
}

ChValue ch_gc_make_group_info(ChGC *gc, ChValue name, uint32_t gid) {
    ch_gc_push(gc, &name);
    ChGroupInfo *gi = (ChGroupInfo *)ch_gc_alloc(gc, sizeof(ChGroupInfo), CH_TAG_GROUP_INFO);
    ch_gc_pop(gc);
    gi->name = name;
    gi->gid = gid;
    return ch_make_pointer(&gi->header);
}
