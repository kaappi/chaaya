#include "chaaya/gc_deep_copy.h"

#include "chaaya/bignum.h"
#include "chaaya/complex.h"
#include "chaaya/fiber.h"
#include "chaaya/rational.h"
#include "chaaya/shared_channel.h"
#include "chaaya/vm.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DC_VISITED_INIT_CAP 64u

typedef struct DcVisitedEntry {
    ChObject *src;
    ChValue dest;
} DcVisitedEntry;

typedef struct DcVisited {
    DcVisitedEntry *entries;
    size_t cap;
    size_t count;
} DcVisited;

static void dc_set_error(ChGC *dest, const char *msg) {
    if (dest && dest->vm) {
        snprintf(dest->vm->error, sizeof(dest->vm->error), "%s", msg);
    }
}

static ChValue dc_reject(ChGC *dest, const char *msg) {
    dc_set_error(dest, msg);
    return CH_UNDEFINED;
}

static bool dc_visited_init(DcVisited *vis) {
    vis->entries = (DcVisitedEntry *)calloc(DC_VISITED_INIT_CAP, sizeof(DcVisitedEntry));
    if (!vis->entries) {
        return false;
    }
    vis->cap = DC_VISITED_INIT_CAP;
    vis->count = 0;
    return true;
}

static void dc_visited_deinit(DcVisited *vis) {
    free(vis->entries);
    vis->entries = NULL;
    vis->cap = 0;
    vis->count = 0;
}

static bool dc_visited_grow(DcVisited *vis) {
    size_t ncap = vis->cap * 2;
    DcVisitedEntry *ne = (DcVisitedEntry *)calloc(ncap, sizeof(DcVisitedEntry));
    if (!ne) {
        return false;
    }
    for (size_t i = 0; i < vis->cap; i++) {
        if (vis->entries[i].src) {
            size_t idx = ((size_t)(uintptr_t)vis->entries[i].src >> 4) & (ncap - 1);
            while (ne[idx].src) {
                idx = (idx + 1) & (ncap - 1);
            }
            ne[idx] = vis->entries[i];
        }
    }
    free(vis->entries);
    vis->entries = ne;
    vis->cap = ncap;
    return true;
}

static bool dc_visited_get(const DcVisited *vis, ChObject *src, ChValue *out) {
    if (vis->cap == 0) {
        return false;
    }
    size_t idx = ((size_t)(uintptr_t)src >> 4) & (vis->cap - 1);
    for (;;) {
        DcVisitedEntry *e = &vis->entries[idx];
        if (!e->src) {
            return false;
        }
        if (e->src == src) {
            *out = e->dest;
            return true;
        }
        idx = (idx + 1) & (vis->cap - 1);
    }
}

static bool dc_visited_put(DcVisited *vis, ChObject *src, ChValue dest) {
    if (vis->count * 2 >= vis->cap && !dc_visited_grow(vis)) {
        return false;
    }
    size_t idx = ((size_t)(uintptr_t)src >> 4) & (vis->cap - 1);
    for (;;) {
        DcVisitedEntry *e = &vis->entries[idx];
        if (!e->src) {
            e->src = src;
            e->dest = dest;
            vis->count++;
            return true;
        }
        if (e->src == src) {
            e->dest = dest;
            return true;
        }
        idx = (idx + 1) & (vis->cap - 1);
    }
}

static ChGC *dc_channel_promote_gc(ChGC *dest, ChObject *obj) {
    if (obj->owner == dest->id) {
        return dest;
    }
    if (dest->vm && dest->vm->parent_vm && obj->owner == dest->vm->parent_vm->gc.id) {
        return &dest->vm->parent_vm->gc;
    }
    return NULL;
}

static ChValue dc_copy_value(ChGC *dest, ChValue src, DcVisited *vis);

static ChValue dc_copy_pair(ChGC *dest, ChPair *pair, DcVisited *vis) {
    ChObject *src_obj = &pair->header;
    ChValue head_new = ch_gc_cons(dest, CH_NIL, CH_NIL);
    if (!dc_visited_put(vis, src_obj, head_new)) {
        return dc_reject(dest, "deep-copy: out of memory");
    }

    ChPair *src_pair = pair;
    ChPair *dst_pair = ch_as_pair(head_new);
    for (;;) {
        ChValue car_copy = dc_copy_value(dest, src_pair->car, vis);
        if (car_copy == CH_UNDEFINED && dest->vm && dest->vm->error[0]) {
            return CH_UNDEFINED;
        }
        dst_pair->car = car_copy;

        ChValue cdr = src_pair->cdr;
        if (!ch_is_pointer(cdr)) {
            dst_pair->cdr = cdr;
            break;
        }
        ChObject *cdr_obj = ch_to_object(cdr);
        if (cdr_obj->tag != CH_TAG_PAIR) {
            ChValue cdr_copy = dc_copy_value(dest, cdr, vis);
            if (cdr_copy == CH_UNDEFINED && dest->vm && dest->vm->error[0]) {
                return CH_UNDEFINED;
            }
            dst_pair->cdr = cdr_copy;
            break;
        }
        ChValue already = CH_UNDEFINED;
        if (dc_visited_get(vis, cdr_obj, &already)) {
            dst_pair->cdr = already;
            break;
        }
        ChValue next_new = ch_gc_cons(dest, CH_NIL, CH_NIL);
        if (!dc_visited_put(vis, cdr_obj, next_new)) {
            return dc_reject(dest, "deep-copy: out of memory");
        }
        dst_pair->cdr = next_new;
        src_pair = (ChPair *)cdr_obj;
        dst_pair = ch_as_pair(next_new);
    }
    return head_new;
}

static ChValue dc_copy_function(ChGC *dest, ChFunction *func, DcVisited *vis) {
    ChObject *src_obj = &func->header;
    ChValue new_v = ch_gc_make_function(dest);
    if (!dc_visited_put(vis, src_obj, new_v)) {
        return dc_reject(dest, "deep-copy: out of memory");
    }

    ChFunction *nf = ch_as_function(new_v);
    nf->arity = func->arity;
    nf->num_regs = func->num_regs;
    nf->num_upvalues = func->num_upvalues;
    nf->variadic = func->variadic;
    nf->code_len = func->code_len;
    nf->const_count = func->const_count;

    if (func->code_len > 0) {
        nf->code = (uint8_t *)malloc(func->code_len);
        if (!nf->code) {
            abort();
        }
        memcpy(nf->code, func->code, func->code_len);
    }

    if (func->num_upvalues > 0) {
        nf->uv_is_local = (uint8_t *)malloc(func->num_upvalues);
        nf->uv_index = (uint8_t *)malloc(func->num_upvalues);
        if (!nf->uv_is_local || !nf->uv_index) {
            abort();
        }
        memcpy(nf->uv_is_local, func->uv_is_local, func->num_upvalues);
        memcpy(nf->uv_index, func->uv_index, func->num_upvalues);
    }

    if (func->const_count > 0) {
        nf->constants = (ChValue *)calloc(func->const_count, sizeof(ChValue));
        if (!nf->constants) {
            abort();
        }
        for (size_t i = 0; i < func->const_count; i++) {
            nf->constants[i] = dc_copy_value(dest, func->constants[i], vis);
            if (nf->constants[i] == CH_UNDEFINED && dest->vm && dest->vm->error[0]) {
                return CH_UNDEFINED;
            }
        }
    }
    return new_v;
}

static void dc_free_upvalues(ChUpvalue **uvs, uint8_t n) {
    if (!uvs) {
        return;
    }
    for (uint8_t i = 0; i < n; i++) {
        free(uvs[i]);
    }
    free(uvs);
}

static ChValue dc_copy_closure(ChGC *dest, ChClosure *cl, DcVisited *vis) {
    ChObject *src_obj = &cl->header;
    ChValue fn_v = ch_make_pointer(&cl->fn->header);
    ChValue new_fn_v = dc_copy_value(dest, fn_v, vis);
    if (new_fn_v == CH_UNDEFINED && dest->vm && dest->vm->error[0]) {
        return CH_UNDEFINED;
    }

    ChFunction *new_fn = ch_as_function(new_fn_v);
    ChUpvalue **uvs = NULL;
    uint8_t num_uv = cl->fn->num_upvalues;
    if (num_uv > 0) {
        uvs = (ChUpvalue **)calloc(num_uv, sizeof(ChUpvalue *));
        if (!uvs) {
            abort();
        }
    }

    ChValue new_v = ch_gc_make_closure(dest, new_fn, uvs);
    if (!dc_visited_put(vis, src_obj, new_v)) {
        dc_free_upvalues(uvs, num_uv);
        return dc_reject(dest, "deep-copy: out of memory");
    }

    ChClosure *new_cl = ch_as_closure(new_v);
    new_cl->home_env = cl->home_env;

    for (uint8_t i = 0; i < num_uv; i++) {
        ChUpvalue *uv = cl->upvalues ? cl->upvalues[i] : NULL;
        if (!uv || !uv->location) {
            dc_free_upvalues(uvs, num_uv);
            new_cl->upvalues = NULL;
            return dc_reject(dest, "deep-copy: missing upvalue");
        }
        /* Snapshot open locations so thread-start! can capture let-bound values. */
        ChValue src_val = uv->is_closed ? uv->closed_value : *uv->location;
        ChUpvalue *copy_uv = (ChUpvalue *)calloc(1, sizeof(ChUpvalue));
        if (!copy_uv) {
            abort();
        }
        copy_uv->is_closed = true;
        copy_uv->location = &copy_uv->closed_value;
        copy_uv->closed_value = dc_copy_value(dest, src_val, vis);
        if (copy_uv->closed_value == CH_UNDEFINED && dest->vm && dest->vm->error[0]) {
            free(copy_uv);
            dc_free_upvalues(uvs, num_uv);
            new_cl->upvalues = NULL;
            return CH_UNDEFINED;
        }
        uvs[i] = copy_uv;
    }
    return new_v;
}

static ChValue dc_copy_hashtable(ChGC *dest, ChHashtable *ht, DcVisited *vis) {
    ChObject *src_obj = &ht->header;
    ChValue new_v = ch_gc_make_hashtable(dest, ht->cap);
    if (!dc_visited_put(vis, src_obj, new_v)) {
        return dc_reject(dest, "deep-copy: out of memory");
    }

    ChHashtable *nht = ch_as_hashtable(new_v);
    nht->mode = ht->mode;
    nht->count = 0;
    for (size_t i = 0; i < ht->cap; i++) {
        if (!ht->used[i]) {
            continue;
        }
        ChValue nk = dc_copy_value(dest, ht->keys[i], vis);
        if (nk == CH_UNDEFINED && dest->vm && dest->vm->error[0]) {
            return CH_UNDEFINED;
        }
        ChValue nv = dc_copy_value(dest, ht->vals[i], vis);
        if (nv == CH_UNDEFINED && dest->vm && dest->vm->error[0]) {
            return CH_UNDEFINED;
        }
        size_t idx = i % nht->cap;
        while (nht->used[idx]) {
            idx = (idx + 1) % nht->cap;
        }
        nht->keys[idx] = nk;
        nht->vals[idx] = nv;
        nht->used[idx] = true;
        nht->count++;
    }
    return new_v;
}

static ChValue dc_copy_record_type(ChGC *dest, ChRecordType *rt, DcVisited *vis) {
    ChObject *src_obj = &rt->header;
    ChValue name = dc_copy_value(dest, rt->name, vis);
    if (name == CH_UNDEFINED && dest->vm && dest->vm->error[0]) {
        return CH_UNDEFINED;
    }

    ChRecordType *new_parent = NULL;
    if (rt->parent) {
        ChValue pv = dc_copy_value(dest, ch_make_pointer(&rt->parent->header), vis);
        if (pv == CH_UNDEFINED && dest->vm && dest->vm->error[0]) {
            return CH_UNDEFINED;
        }
        new_parent = ch_as_record_type(pv);
    }

    uint16_t own_fields =
        rt->parent ? (uint16_t)(rt->num_fields - rt->parent->num_fields) : rt->num_fields;
    ChValue new_v;
    if (rt->parent) {
        new_v = ch_gc_make_record_type_ext(dest, name, own_fields, new_parent);
    } else {
        new_v = ch_gc_make_record_type(dest, name, rt->num_fields);
    }
    if (new_v == CH_UNDEFINED) {
        return dc_reject(dest, "deep-copy: record type allocation failed");
    }
    if (!dc_visited_put(vis, src_obj, new_v)) {
        return dc_reject(dest, "deep-copy: out of memory");
    }
    return new_v;
}

static ChValue dc_copy_record(ChGC *dest, ChRecord *rec, DcVisited *vis) {
    ChObject *src_obj = &rec->header;
    ChValue rt_v = dc_copy_value(dest, ch_make_pointer(&rec->rtype->header), vis);
    if (rt_v == CH_UNDEFINED && dest->vm && dest->vm->error[0]) {
        return CH_UNDEFINED;
    }
    ChRecordType *new_rt = ch_as_record_type(rt_v);

    ChValue *fields = NULL;
    if (rec->num_fields > 0) {
        fields = (ChValue *)calloc(rec->num_fields, sizeof(ChValue));
        if (!fields) {
            abort();
        }
        for (uint16_t i = 0; i < rec->num_fields; i++) {
            fields[i] = dc_copy_value(dest, rec->fields[i], vis);
            if (fields[i] == CH_UNDEFINED && dest->vm && dest->vm->error[0]) {
                free(fields);
                return CH_UNDEFINED;
            }
        }
    }

    ChValue new_v = ch_gc_make_record(dest, new_rt, fields, rec->num_fields);
    free(fields);
    if (!dc_visited_put(vis, src_obj, new_v)) {
        return dc_reject(dest, "deep-copy: out of memory");
    }
    return new_v;
}

static ChValue dc_copy_value(ChGC *dest, ChValue src, DcVisited *vis) {
    if (!ch_is_pointer(src)) {
        return src;
    }

    ChObject *obj = ch_to_object(src);
    ChValue already = CH_UNDEFINED;
    if (dc_visited_get(vis, obj, &already)) {
        return already;
    }

    switch ((ChObjectTag)obj->tag) {
    case CH_TAG_PAIR:
        return dc_copy_pair(dest, (ChPair *)obj, vis);

    case CH_TAG_SYMBOL: {
        ChSymbol *sym = (ChSymbol *)obj;
        if (ch_symbol_is_interned(sym)) {
            return ch_gc_intern_symbol(dest, sym->name, sym->len);
        }
        ChValue new_v = ch_gc_alloc_uninterned_symbol(dest, sym->name, sym->len);
        if (!dc_visited_put(vis, obj, new_v)) {
            return dc_reject(dest, "deep-copy: out of memory");
        }
        return new_v;
    }

    case CH_TAG_STRING: {
        ChString *s = (ChString *)obj;
        ChValue new_v = ch_gc_make_string(dest, s->data, s->len);
        if (!dc_visited_put(vis, obj, new_v)) {
            return dc_reject(dest, "deep-copy: out of memory");
        }
        return new_v;
    }

    case CH_TAG_VECTOR: {
        ChVector *vec = (ChVector *)obj;
        ChValue new_v = ch_gc_make_vector(dest, vec->len, CH_UNDEFINED);
        if (!dc_visited_put(vis, obj, new_v)) {
            return dc_reject(dest, "deep-copy: out of memory");
        }
        ChVector *nvec = ch_as_vector(new_v);
        for (size_t i = 0; i < vec->len; i++) {
            nvec->items[i] = dc_copy_value(dest, vec->items[i], vis);
            if (nvec->items[i] == CH_UNDEFINED && dest->vm && dest->vm->error[0]) {
                return CH_UNDEFINED;
            }
        }
        return new_v;
    }

    case CH_TAG_BYTEVECTOR: {
        ChBytevector *bv = (ChBytevector *)obj;
        ChValue new_v = ch_gc_make_bytevector(dest, bv->len, 0);
        if (!dc_visited_put(vis, obj, new_v)) {
            return dc_reject(dest, "deep-copy: out of memory");
        }
        ChBytevector *nbv = ch_as_bytevector(new_v);
        if (bv->len > 0) {
            memcpy(nbv->data, bv->data, bv->len);
        }
        return new_v;
    }

    case CH_TAG_BIGNUM: {
        ChBignum *bn = (ChBignum *)obj;
        ChValue new_v =
            ch_gc_make_bignum_from_limbs(dest, bn->limbs, bn->len, bn->positive);
        if (!dc_visited_put(vis, obj, new_v)) {
            return dc_reject(dest, "deep-copy: out of memory");
        }
        return new_v;
    }

    case CH_TAG_RATIONAL: {
        ChRational *r = (ChRational *)obj;
        ChValue num = dc_copy_value(dest, r->numerator, vis);
        if (num == CH_UNDEFINED && dest->vm && dest->vm->error[0]) {
            return CH_UNDEFINED;
        }
        ChValue den = dc_copy_value(dest, r->denominator, vis);
        if (den == CH_UNDEFINED && dest->vm && dest->vm->error[0]) {
            return CH_UNDEFINED;
        }
        ChValue new_v = ch_make_rational(dest, num, den);
        if (new_v == CH_UNDEFINED) {
            return dc_reject(dest, "deep-copy: rational copy failed");
        }
        if (!dc_visited_put(vis, obj, new_v)) {
            return dc_reject(dest, "deep-copy: out of memory");
        }
        return new_v;
    }

    case CH_TAG_COMPLEX: {
        ChComplex *c = (ChComplex *)obj;
        ChValue new_v =
            ch_make_complex_ex(dest, c->real, c->imag, c->exact_real, c->exact_imag);
        if (!dc_visited_put(vis, obj, new_v)) {
            return dc_reject(dest, "deep-copy: out of memory");
        }
        return new_v;
    }

    case CH_TAG_FUNCTION:
        return dc_copy_function(dest, (ChFunction *)obj, vis);

    case CH_TAG_CLOSURE:
        return dc_copy_closure(dest, (ChClosure *)obj, vis);

    case CH_TAG_NATIVE: {
        ChNative *n = (ChNative *)obj;
        ChValue new_v = ch_gc_make_native(dest, n->fn, n->name, n->arity, n->min_arity);
        if (!dc_visited_put(vis, obj, new_v)) {
            return dc_reject(dest, "deep-copy: out of memory");
        }
        return new_v;
    }

    case CH_TAG_VALUES: {
        ChValues *vs = (ChValues *)obj;
        ChValue *tmp = NULL;
        if (vs->count > 0) {
            tmp = (ChValue *)calloc(vs->count, sizeof(ChValue));
            if (!tmp) {
                abort();
            }
            for (size_t i = 0; i < vs->count; i++) {
                tmp[i] = dc_copy_value(dest, vs->items[i], vis);
                if (tmp[i] == CH_UNDEFINED && dest->vm && dest->vm->error[0]) {
                    free(tmp);
                    return CH_UNDEFINED;
                }
            }
        }
        ChValue new_v = ch_gc_make_values(dest, tmp, vs->count);
        free(tmp);
        if (!dc_visited_put(vis, obj, new_v)) {
            return dc_reject(dest, "deep-copy: out of memory");
        }
        return new_v;
    }

    case CH_TAG_PROMISE: {
        ChPromise *pr = (ChPromise *)obj;
        ChValue new_v = ch_gc_make_promise(dest, pr->forced, CH_NIL);
        if (!dc_visited_put(vis, obj, new_v)) {
            return dc_reject(dest, "deep-copy: out of memory");
        }
        ChPromise *npr = ch_as_promise(new_v);
        npr->forcing = 0;
        npr->value = dc_copy_value(dest, pr->value, vis);
        if (npr->value == CH_UNDEFINED && dest->vm && dest->vm->error[0]) {
            return CH_UNDEFINED;
        }
        return new_v;
    }

    case CH_TAG_PARAMETER: {
        ChParameter *p = (ChParameter *)obj;
        ChValue init = dc_copy_value(dest, p->init, vis);
        if (init == CH_UNDEFINED && dest->vm && dest->vm->error[0]) {
            return CH_UNDEFINED;
        }
        ChValue converter = dc_copy_value(dest, p->converter, vis);
        if (converter == CH_UNDEFINED && dest->vm && dest->vm->error[0]) {
            return CH_UNDEFINED;
        }
        ChValue new_v = ch_gc_make_parameter(dest, init, converter);
        if (!dc_visited_put(vis, obj, new_v)) {
            return dc_reject(dest, "deep-copy: out of memory");
        }
        ChParameter *np = ch_as_parameter(new_v);
        np->value = dc_copy_value(dest, p->value, vis);
        if (np->value == CH_UNDEFINED && dest->vm && dest->vm->error[0]) {
            return CH_UNDEFINED;
        }
        return new_v;
    }

    case CH_TAG_ERROR_OBJ: {
        ChErrorObject *e = (ChErrorObject *)obj;
        ChValue msg = dc_copy_value(dest, e->message, vis);
        if (msg == CH_UNDEFINED && dest->vm && dest->vm->error[0]) {
            return CH_UNDEFINED;
        }
        ChValue irritants = dc_copy_value(dest, e->irritants, vis);
        if (irritants == CH_UNDEFINED && dest->vm && dest->vm->error[0]) {
            return CH_UNDEFINED;
        }
        ChValue new_v = ch_gc_make_error_object(dest, msg, irritants, e->error_type);
        if (!dc_visited_put(vis, obj, new_v)) {
            return dc_reject(dest, "deep-copy: out of memory");
        }
        return new_v;
    }

    case CH_TAG_HASHTABLE:
        return dc_copy_hashtable(dest, (ChHashtable *)obj, vis);

    case CH_TAG_RECORD_TYPE:
        return dc_copy_record_type(dest, (ChRecordType *)obj, vis);

    case CH_TAG_RECORD:
        return dc_copy_record(dest, (ChRecord *)obj, vis);

    case CH_TAG_TIME: {
        ChTime *t = (ChTime *)obj;
        ChValue type_sym = dc_copy_value(dest, t->type_sym, vis);
        if (type_sym == CH_UNDEFINED && dest->vm && dest->vm->error[0]) {
            return CH_UNDEFINED;
        }
        ChValue new_v = ch_gc_make_time(dest, t->seconds, t->nanoseconds, type_sym);
        if (!dc_visited_put(vis, obj, new_v)) {
            return dc_reject(dest, "deep-copy: out of memory");
        }
        return new_v;
    }

    case CH_TAG_RANDOM_SOURCE: {
        ChRandomSource *rs = (ChRandomSource *)obj;
        ChValue new_v = ch_gc_make_random_source(dest, 0);
        ChRandomSource *nrs = ch_as_random_source(new_v);
        nrs->s[0] = rs->s[0];
        nrs->s[1] = rs->s[1];
        nrs->s[2] = rs->s[2];
        nrs->s[3] = rs->s[3];
        if (!dc_visited_put(vis, obj, new_v)) {
            return dc_reject(dest, "deep-copy: out of memory");
        }
        return new_v;
    }

    case CH_TAG_CHANNEL: {
        ChChannel *ch = (ChChannel *)obj;
        ChSharedChannel *sc = NULL;
        if (ch->shared) {
            sc = (ChSharedChannel *)ch->shared;
        } else {
            ChGC *promote_gc = dc_channel_promote_gc(dest, obj);
            if (!promote_gc) {
                return dc_reject(dest, "deep-copy: channel belongs to another OS thread");
            }
            if (ch_channel_promote(promote_gc, ch) != 0) {
                if (dest->vm && promote_gc->vm && promote_gc->vm != dest->vm &&
                    promote_gc->vm->error[0]) {
                    snprintf(dest->vm->error, sizeof(dest->vm->error), "%s",
                             promote_gc->vm->error);
                } else if (dest->vm && dest->vm->error[0] == '\0') {
                    dc_set_error(dest, "deep-copy: channel promotion failed");
                }
                return CH_UNDEFINED;
            }
            sc = (ChSharedChannel *)ch->shared;
        }
        if (!sc) {
            return dc_reject(dest, "deep-copy: channel promotion failed");
        }
        ChValue stub = ch_shared_channel_alloc_stub(dest, sc, ch->capacity,
                                                     ch->rendezvous ? 1 : 0, ch->closed);
        if (stub == CH_UNDEFINED) {
            return dc_reject(dest, "deep-copy: channel stub allocation failed");
        }
        ch_shared_channel_retain(sc);
        if (!dc_visited_put(vis, obj, stub)) {
            ch_shared_channel_release(sc);
            return dc_reject(dest, "deep-copy: out of memory");
        }
        return stub;
    }

    case CH_TAG_PORT:
        return dc_reject(dest, "deep-copy: port");
    case CH_TAG_CONTINUATION:
        return dc_reject(dest, "deep-copy: continuation");
    case CH_TAG_FIBER:
        return dc_reject(dest, "deep-copy: fiber");
    case CH_TAG_ENVIRONMENT:
        return dc_reject(dest, "deep-copy: environment");
    case CH_TAG_TRANSFORMER:
        return dc_reject(dest, "deep-copy: transformer");
    case CH_TAG_FOREIGN_LIBRARY:
        return dc_reject(dest, "deep-copy: foreign library");
    case CH_TAG_FOREIGN_PROC:
        return dc_reject(dest, "deep-copy: foreign procedure");
    case CH_TAG_EPHEMERON:
        return dc_reject(dest, "deep-copy: ephemeron");
    case CH_TAG_FILE_INFO:
        return dc_reject(dest, "deep-copy: file-info");
    case CH_TAG_GROUP_INFO:
        return dc_reject(dest, "deep-copy: group-info");

    default:
        return dc_reject(dest, "deep-copy: unsupported heap type");
    }
}

ChValue ch_gc_deep_copy(ChGC *dest, ChValue src) {
    if (!dest) {
        return CH_UNDEFINED;
    }
    if (dest->vm) {
        dest->vm->error[0] = '\0';
    }

    DcVisited vis;
    if (!dc_visited_init(&vis)) {
        return dc_reject(dest, "deep-copy: out of memory");
    }

    /* In-progress dest objects are not on the root stack; suppress GC for the
     * walk so a long list spine cannot be collected mid-copy (#801). */
    dest->no_collect++;
    ChValue out = dc_copy_value(dest, src, &vis);
    dest->no_collect--;
    dc_visited_deinit(&vis);
    return out;
}
