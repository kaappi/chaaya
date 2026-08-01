#include "chaaya/gc.h"
#include "chaaya/vm.h"
#include "chaaya/library.h"
#include "chaaya/environment.h"
#include "chaaya/ffi.h"
#include "chaaya/fiber.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void ch_gc_init(ChGC *gc) {
    memset(gc, 0, sizeof(*gc));
    gc->threshold = CH_GC_DEFAULT_THRESHOLD;
    gc->promotion_age = CH_GC_DEFAULT_PROMOTION_AGE;
    gc->major_interval = CH_GC_DEFAULT_MAJOR_INTERVAL;
    gc->symbol_cap = 64;
    gc->symbols = (ChSymbol **)calloc(gc->symbol_cap, sizeof(ChSymbol *));
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
    free(gc->symbols);
    free(gc->pending_ephemerons);
    memset(gc, 0, sizeof(*gc));
}

void ch_gc_push(ChGC *gc, ChValue *slot) {
    if (gc->root_count >= CH_GC_ROOT_MAX) {
        abort();
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

static void mark_object(ChObject *obj) {
    if (!obj || obj->marked) {
        return;
    }
    obj->marked = 1;
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
        break;
    }
    case CH_TAG_RECORD_TYPE: {
        ChRecordType *rt = (ChRecordType *)obj;
        mark_value(rt->name);
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
        break;
    }
    case CH_TAG_CHANNEL: {
        ChChannel *channel = (ChChannel *)obj;
        if (channel->storage_cap == 0 || !channel->items) {
            break;
        }
        for (size_t i = 0; i < channel->count; i++) {
            size_t idx = (channel->head + i) % channel->storage_cap;
            mark_value(channel->items[idx]);
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
    }
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
    }
    for (size_t i = 0; i < gc->compiling_fn_depth; i++) {
        if (gc->compiling_fns[i]) {
            mark_object(&gc->compiling_fns[i]->header);
        }
    }
    for (size_t i = 0; i < gc->root_count; i++) {
        mark_value(*gc->roots[i]);
    }
    for (size_t i = 0; i < gc->symbol_count; i++) {
        if (gc->symbols[i]) {
            mark_object(&gc->symbols[i]->header);
        }
    }
    process_weak_refs(gc);
    g_mark_gc = NULL;
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
            continue;
        }
        link = &obj->next;
    }
}

void ch_gc_collect_minor(ChGC *gc) {
    mark_roots_and_symbols(gc);
    for (ChObject *obj = gc->old_objects; obj; obj = obj->next) {
        mark_object(obj);
    }
    sweep_young_minor(gc);
    clear_marks(gc->old_objects);
    gc->alloc_count = 0;
    gc->minor_collections++;
    gc->collections++;
    gc->threshold = gc->object_count * 2 + CH_GC_DEFAULT_THRESHOLD;
}

void ch_gc_collect_major(ChGC *gc) {
    mark_roots_and_symbols(gc);
    sweep_list_major(gc, &gc->young_objects, &gc->young_count);
    sweep_list_major(gc, &gc->old_objects, &gc->old_count);
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
    ch_gc_promote_to_old(gc, child);
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
}

void *ch_gc_alloc(ChGC *gc, size_t size, ChObjectTag tag) {
    if (gc->alloc_count >= gc->threshold) {
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
    return ch_make_pointer(&tr->header);
}

ChValue ch_gc_make_record_type(ChGC *gc, ChValue name, uint16_t num_fields) {
    ch_gc_push(gc, &name);
    ChRecordType *rt = (ChRecordType *)ch_gc_alloc(gc, sizeof(ChRecordType), CH_TAG_RECORD_TYPE);
    ch_gc_pop(gc);
    rt->name = name;
    rt->num_fields = num_fields;
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

ChValue ch_gc_make_file_port(ChGC *gc, FILE *file, int input, int output) {
    ChPort *p = (ChPort *)ch_gc_alloc(gc, sizeof(ChPort), CH_TAG_PORT);
    init_port_common(p, CH_PORT_FILE, input, output);
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
    *fi = *info;
    return ch_make_pointer(&fi->header);
}
