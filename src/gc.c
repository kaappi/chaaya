#include "chaaya/gc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void ch_gc_init(ChGC *gc) {
    memset(gc, 0, sizeof(*gc));
    gc->threshold = CH_GC_DEFAULT_THRESHOLD;
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
        if (p->kind != CH_PORT_STDIO && p->kind != CH_PORT_FILE && p->buf) {
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
    default:
        break;
    }
    free(obj);
}

void ch_gc_deinit(ChGC *gc) {
    ChObject *obj = gc->objects;
    while (obj) {
        ChObject *next = obj->next;
        free_object(obj);
        obj = next;
    }
    free(gc->symbols);
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

static void mark_value(ChValue v);

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
    }
}

static void mark_value(ChValue v) {
    if (ch_is_pointer(v)) {
        mark_object(ch_to_object(v));
    }
}

void ch_gc_collect(ChGC *gc) {
    for (size_t i = 0; i < gc->root_count; i++) {
        mark_value(*gc->roots[i]);
    }
    for (size_t i = 0; i < gc->symbol_count; i++) {
        if (gc->symbols[i]) {
            mark_object(&gc->symbols[i]->header);
        }
    }

    ChObject **link = &gc->objects;
    while (*link) {
        ChObject *obj = *link;
        if (!obj->marked) {
            *link = obj->next;
            free_object(obj);
            gc->object_count--;
        } else {
            obj->marked = 0;
            link = &obj->next;
        }
    }
    gc->alloc_count = 0;
    gc->collections++;
    gc->threshold = gc->object_count * 2 + CH_GC_DEFAULT_THRESHOLD;
}

void *ch_gc_alloc(ChGC *gc, size_t size, ChObjectTag tag) {
    if (gc->alloc_count >= gc->threshold) {
        ch_gc_collect(gc);
    }
    ChObject *obj = (ChObject *)calloc(1, size);
    if (!obj) {
        abort();
    }
    obj->tag = (uint8_t)tag;
    obj->marked = 0;
    obj->next = gc->objects;
    gc->objects = obj;
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
    ChString *s = (ChString *)ch_gc_alloc(gc, sizeof(ChString) + len + 1, CH_TAG_STRING);
    s->len = len;
    memcpy(s->data, bytes, len);
    s->data[len] = '\0';
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

ChValue ch_gc_make_stdio_port(ChGC *gc, FILE *file, int input, int output) {
    ChPort *p = (ChPort *)ch_gc_alloc(gc, sizeof(ChPort), CH_TAG_PORT);
    p->kind = CH_PORT_STDIO;
    p->input = (uint8_t)(input ? 1 : 0);
    p->output = (uint8_t)(output ? 1 : 0);
    p->closed = 0;
    p->file = file;
    return ch_make_pointer(&p->header);
}

ChValue ch_gc_make_string_input_port(ChGC *gc, const char *bytes, size_t len) {
    ChPort *p = (ChPort *)ch_gc_alloc(gc, sizeof(ChPort), CH_TAG_PORT);
    p->kind = CH_PORT_STRING_IN;
    p->input = 1;
    p->output = 0;
    p->closed = 0;
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
    p->kind = CH_PORT_STRING_OUT;
    p->input = 0;
    p->output = 1;
    p->closed = 0;
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

ChValue ch_gc_make_transformer(ChGC *gc) {
    ChTransformer *tr = (ChTransformer *)ch_gc_alloc(gc, sizeof(ChTransformer), CH_TAG_TRANSFORMER);
    tr->literal_count = 0;
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
    p->kind = CH_PORT_FILE;
    p->input = (uint8_t)(input ? 1 : 0);
    p->output = (uint8_t)(output ? 1 : 0);
    p->closed = 0;
    p->file = file;
    p->buf = NULL;
    p->len = 0;
    p->cap = 0;
    p->pos = 0;
    return ch_make_pointer(&p->header);
}
