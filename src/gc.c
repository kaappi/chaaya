#include "chaaya/gc.h"

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
    case CH_TAG_SYMBOL:
    case CH_TAG_STRING:
    case CH_TAG_NATIVE:
        break;
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
