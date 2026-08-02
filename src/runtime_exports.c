#include "chaaya/runtime_exports.h"

#include "chaaya/compiler.h"
#include "chaaya/eval.h"
#include "chaaya/gc.h"
#include "chaaya/reader.h"
#include "chaaya/value.h"
#include "chaaya/vm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static ChVM *g_rt_vm = NULL;
static ChVM g_rt_vm_storage;
static int g_rt_vm_live = 0;

static void rt_die(const char *msg) {
    fprintf(stderr, "chaaya runtime: %s\n", msg);
    exit(1);
}

static void rt_die_vm(ChVM *vm, const char *ctx) {
    fprintf(stderr, "chaaya runtime: %s: %s\n", ctx, vm && vm->error[0] ? vm->error : "error");
    exit(1);
}

int ch_rt_native_arch_supported(void) {
#if defined(__aarch64__) || defined(__x86_64__) || defined(_M_X64) || defined(_M_ARM64)
    return 1;
#else
    return 0;
#endif
}

ChVM *ch_rt_init(void) {
    if (g_rt_vm_live) {
        return g_rt_vm;
    }
    ch_vm_init(&g_rt_vm_storage);
    ch_vm_register_primitives(&g_rt_vm_storage);
    g_rt_vm = &g_rt_vm_storage;
    g_rt_vm_live = 1;
    return g_rt_vm;
}

void ch_rt_deinit(ChVM *vm) {
    if (!vm || !g_rt_vm_live || vm != g_rt_vm) {
        return;
    }
    ch_vm_deinit(vm);
    g_rt_vm = NULL;
    g_rt_vm_live = 0;
}

void ch_rt_set_argv(ChVM *vm, char **argv) {
    if (!vm || !argv) {
        return;
    }
    vm->script_arg_count = 0;
    if (argv[0]) {
        vm->script_path = argv[0];
    }
    for (size_t i = 1; argv[i] && vm->script_arg_count < CH_VM_MAX_SCRIPT_ARGS; i++) {
        vm->script_args[vm->script_arg_count++] = argv[i];
    }
}

static int lookup_global_idx(ChVM *vm, const char *name, size_t name_len, int create) {
    ChValue symv = ch_gc_intern_symbol(&vm->gc, name, name_len);
    ChSymbol *sym = ch_as_symbol(symv);
    if (!create) {
        for (size_t i = 0; i < vm->global_count; i++) {
            if (vm->globals[i].name == sym) {
                return (int)i;
            }
        }
        return -1;
    }
    return ch_vm_intern_global(vm, sym);
}

uint64_t ch_rt_global_lookup(ChVM *vm, const char *name, uint64_t name_len) {
    if (!vm || !name) {
        rt_die("global_lookup: null");
    }
    int idx = lookup_global_idx(vm, name, (size_t)name_len, 0);
    if (idx < 0 || !vm->globals[idx].defined) {
        fprintf(stderr, "chaaya runtime: undefined variable: %.*s\n", (int)name_len, name);
        exit(1);
    }
    return vm->globals[idx].value;
}

void ch_rt_define_global(ChVM *vm, const char *name, uint64_t name_len, uint64_t value) {
    if (!vm || !name) {
        rt_die("define_global: null");
    }
    int idx = lookup_global_idx(vm, name, (size_t)name_len, 1);
    if (idx < 0) {
        rt_die("define_global: table full");
    }
    ch_vm_define_global(vm, idx, (ChValue)value);
}

void ch_rt_set_global(ChVM *vm, const char *name, uint64_t name_len, uint64_t value) {
    if (!vm || !name) {
        rt_die("set_global: null");
    }
    int idx = lookup_global_idx(vm, name, (size_t)name_len, 0);
    if (idx < 0 || !vm->globals[idx].defined) {
        fprintf(stderr, "chaaya runtime: set!: unbound variable '%.*s'\n", (int)name_len, name);
        exit(1);
    }
    ch_vm_define_global(vm, idx, (ChValue)value);
}

uint64_t ch_rt_eval(ChVM *vm, const char *src, uint64_t src_len) {
    if (!vm || !src) {
        rt_die("eval: null");
    }
    ChValue last = CH_VOID;
    ChReader reader;
    ch_reader_init(&reader, &vm->gc, src, (size_t)src_len);
    for (;;) {
        ChValue expr = CH_NIL;
        ch_gc_push(&vm->gc, &expr);
        ChReadStatus st = ch_read_datum(&reader, &expr);
        if (st == CH_READ_EOF) {
            ch_gc_pop(&vm->gc);
            break;
        }
        if (st != CH_READ_OK) {
            ch_gc_pop(&vm->gc);
            rt_die_vm(vm, ch_reader_error(&reader));
        }
        ChValue result = CH_VOID;
        ch_gc_push(&vm->gc, &result);
        if (ch_eval_datum(vm, expr, CH_VOID, &result) != 0) {
            ch_gc_pop_n(&vm->gc, 2);
            rt_die_vm(vm, "eval");
        }
        last = result;
        ch_gc_pop_n(&vm->gc, 2);
    }
    return last;
}

uint64_t ch_rt_eval_cached(ChVM *vm, const char *src, uint64_t src_len, uint64_t *slot) {
    if (!vm || !src || !slot) {
        rt_die("eval_cached: null");
    }
    if (*slot != 0) {
        ChValue fn_val = (ChValue)*slot;
        if (!ch_is_function(fn_val)) {
            return ch_rt_eval(vm, src, src_len);
        }
        ChFunction *fn = (ChFunction *)ch_to_object(fn_val);
        ChValue result = CH_VOID;
        ch_gc_push(&vm->gc, &result);
        if (ch_vm_eval_function(vm, fn, &result) != CH_VM_OK) {
            ch_gc_pop(&vm->gc);
            rt_die_vm(vm, "eval_cached");
        }
        ch_gc_pop(&vm->gc);
        return result;
    }

    ChCompiler compiler;
    ch_compiler_init(&compiler, vm);
    ChReader reader;
    ch_reader_init(&reader, &vm->gc, src, (size_t)src_len);
    ChValue expr = CH_NIL;
    ch_gc_push(&vm->gc, &expr);
    if (ch_read_datum(&reader, &expr) != CH_READ_OK) {
        ch_gc_pop(&vm->gc);
        return ch_rt_eval(vm, src, src_len);
    }
    ChFunction *fn = NULL;
    if (ch_compile_toplevel(&compiler, expr, &fn) != CH_COMPILE_OK) {
        ch_gc_pop(&vm->gc);
        return ch_rt_eval(vm, src, src_len);
    }
    ChValue fn_val = ch_make_pointer(&fn->header);
    ch_gc_add_extra_root(&vm->gc, fn_val);
    *slot = fn_val;
    ch_gc_pop(&vm->gc);

    ChValue result = CH_VOID;
    ch_gc_push(&vm->gc, &result);
    if (ch_vm_eval_function(vm, fn, &result) != CH_VM_OK) {
        ch_gc_pop(&vm->gc);
        rt_die_vm(vm, "eval_cached");
    }
    ch_gc_pop(&vm->gc);
    return result;
}

uint64_t ch_rt_quote_cached(ChVM *vm, const char *src, uint64_t src_len, uint64_t *slot) {
    if (!vm || !src || !slot) {
        rt_die("quote_cached: null");
    }
    if (*slot != 0) {
        return *slot;
    }
    uint64_t val = ch_rt_eval(vm, src, src_len);
    ch_gc_add_extra_root(&vm->gc, (ChValue)val);
    *slot = val;
    return val;
}

uint64_t ch_rt_call_scheme(ChVM *vm, uint64_t proc, uint64_t *args, uint64_t nargs) {
    if (!vm) {
        rt_die("call_scheme: null");
    }
    ChValue result = CH_VOID;
    ChValue *argv = NULL;
    ChValue stack_args[16];
    if (nargs > 16) {
        argv = (ChValue *)malloc(sizeof(ChValue) * (size_t)nargs);
        if (!argv) {
            rt_die("call_scheme: OOM");
        }
    } else {
        argv = stack_args;
    }
    for (uint64_t i = 0; i < nargs; i++) {
        argv[i] = (ChValue)args[i];
    }
    ch_gc_push(&vm->gc, &result);
    ChVMStatus st = ch_vm_apply(vm, (ChValue)proc, argv, (int)nargs, &result);
    ch_gc_pop(&vm->gc);
    if (nargs > 16) {
        free(argv);
    }
    if (st != CH_VM_OK) {
        rt_die_vm(vm, "call_scheme");
    }
    return result;
}

uint64_t ch_rt_apply(ChVM *vm, uint64_t proc, uint64_t args_list) {
    if (!vm) {
        rt_die("apply: null");
    }
    ChValue args_buf[CH_VM_MAX_PENDING_ARGS];
    int nargs = 0;
    ChValue it = (ChValue)args_list;
    while (ch_is_pair(it)) {
        if (nargs >= CH_VM_MAX_PENDING_ARGS) {
            rt_die("apply: too many arguments");
        }
        args_buf[nargs++] = ch_car(it);
        it = ch_cdr(it);
    }
    if (!ch_is_nil(it)) {
        rt_die("apply: improper list");
    }
    return ch_rt_call_scheme(vm, proc, (uint64_t *)args_buf, (uint64_t)nargs);
}

uint64_t ch_rt_cons(ChVM *vm, uint64_t car, uint64_t cdr) {
    if (!vm) {
        rt_die("cons: null");
    }
    return ch_gc_cons(&vm->gc, (ChValue)car, (ChValue)cdr);
}

uint64_t ch_rt_car(uint64_t pair) {
    return ch_car((ChValue)pair);
}

uint64_t ch_rt_cdr(uint64_t pair) {
    return ch_cdr((ChValue)pair);
}

uint64_t ch_rt_make_string(ChVM *vm, const char *bytes, uint64_t len) {
    if (!vm) {
        rt_die("make_string: null");
    }
    return ch_gc_make_string(&vm->gc, bytes, (size_t)len);
}

uint64_t ch_rt_intern_symbol(ChVM *vm, const char *name, uint64_t name_len) {
    if (!vm) {
        rt_die("intern_symbol: null");
    }
    return ch_gc_intern_symbol(&vm->gc, name, (size_t)name_len);
}

static uint64_t call_global_binop(const char *name, uint64_t a, uint64_t b) {
    ChVM *vm = g_rt_vm ? g_rt_vm : ch_rt_init();
    uint64_t proc = ch_rt_global_lookup(vm, name, (uint64_t)strlen(name));
    uint64_t args[2] = {a, b};
    return ch_rt_call_scheme(vm, proc, args, 2);
}

uint64_t ch_rt_fixnum_add(uint64_t a, uint64_t b) {
    if (ch_is_fixnum((ChValue)a) && ch_is_fixnum((ChValue)b)) {
        int64_t x = ch_to_fixnum((ChValue)a);
        int64_t y = ch_to_fixnum((ChValue)b);
        if ((y > 0 && x > CH_FIXNUM_MAX - y) || (y < 0 && x < CH_FIXNUM_MIN - y)) {
            return call_global_binop("+", a, b);
        }
        return ch_make_fixnum(x + y);
    }
    return call_global_binop("+", a, b);
}

uint64_t ch_rt_fixnum_sub(uint64_t a, uint64_t b) {
    if (ch_is_fixnum((ChValue)a) && ch_is_fixnum((ChValue)b)) {
        int64_t x = ch_to_fixnum((ChValue)a);
        int64_t y = ch_to_fixnum((ChValue)b);
        if ((y > 0 && x < CH_FIXNUM_MIN + y) || (y < 0 && x > CH_FIXNUM_MAX + y)) {
            return call_global_binop("-", a, b);
        }
        return ch_make_fixnum(x - y);
    }
    return call_global_binop("-", a, b);
}

uint64_t ch_rt_fixnum_mul(uint64_t a, uint64_t b) {
    if (ch_is_fixnum((ChValue)a) && ch_is_fixnum((ChValue)b)) {
        int64_t x = ch_to_fixnum((ChValue)a);
        int64_t y = ch_to_fixnum((ChValue)b);
        if (x == 0 || y == 0) {
            return ch_make_fixnum(0);
        }
        if (x > 0) {
            if ((y > 0 && x > CH_FIXNUM_MAX / y) || (y < 0 && y < CH_FIXNUM_MIN / x)) {
                return call_global_binop("*", a, b);
            }
        } else {
            if ((y > 0 && x < CH_FIXNUM_MIN / y) || (y < 0 && x < CH_FIXNUM_MAX / y)) {
                return call_global_binop("*", a, b);
            }
        }
        return ch_make_fixnum(x * y);
    }
    return call_global_binop("*", a, b);
}

uint64_t ch_rt_fixnum_lt(uint64_t a, uint64_t b) {
    if (ch_is_fixnum((ChValue)a) && ch_is_fixnum((ChValue)b)) {
        return ch_to_fixnum((ChValue)a) < ch_to_fixnum((ChValue)b) ? CH_TRUE : CH_FALSE;
    }
    return call_global_binop("<", a, b);
}

uint64_t ch_rt_fixnum_eq(uint64_t a, uint64_t b) {
    if (ch_is_fixnum((ChValue)a) && ch_is_fixnum((ChValue)b)) {
        return ch_to_fixnum((ChValue)a) == ch_to_fixnum((ChValue)b) ? CH_TRUE : CH_FALSE;
    }
    return call_global_binop("=", a, b);
}

int ch_rt_run_source(const char *src, size_t len) {
    ChVM *vm = ch_rt_init();
    (void)ch_rt_eval(vm, src, (uint64_t)len);
    ch_rt_deinit(vm);
    return 0;
}

int ch_rt_run_file(const char *path) {
    size_t len = 0;
    char *src = ch_read_file(path, &len);
    if (!src) {
        fprintf(stderr, "chaaya runtime: cannot read '%s'\n", path);
        return 1;
    }
    int rc = ch_rt_run_source(src, len);
    free(src);
    return rc;
}

/* Fallback entry when generated IR cannot lower the program fully. */
static const char *g_embedded_source = NULL;
static size_t g_embedded_source_len = 0;

void ch_rt_set_embedded_source(const char *src, size_t len) {
    g_embedded_source = src;
    g_embedded_source_len = len;
}

int ch_rt_main(void) {
    if (g_embedded_source && g_embedded_source_len > 0) {
        return ch_rt_run_source(g_embedded_source, g_embedded_source_len);
    }
    const char *path = getenv("CHAAYA_RT_SOURCE");
    if (path && path[0]) {
        return ch_rt_run_file(path);
    }
    return 0;
}
