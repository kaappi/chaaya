#include "chaaya/prim.h"

#include <fcntl.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#if defined(__APPLE__) || defined(__FreeBSD__)
#include <stdlib.h>
#endif

static void define_prim(ChVM *vm, const char *name, ChNativeFn fn, int arity, int min_arity) {
    ChValue sym = ch_gc_intern_symbol_cstr(&vm->gc, name);
    int idx = ch_vm_intern_global(vm, ch_as_symbol(sym));
    ChValue nv = ch_gc_make_native(&vm->gc, fn, name, arity, min_arity);
    ch_vm_define_global(vm, idx, nv);
}

static _Atomic uint64_t gensym_counter = 0;

static int read_os_random(uint64_t out[2]) {
#if defined(__APPLE__) || defined(__FreeBSD__)
    arc4random_buf(out, sizeof(uint64_t) * 2);
    return 1;
#else
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) {
        return 0;
    }
    ssize_t n = read(fd, out, sizeof(uint64_t) * 2);
    close(fd);
    return n == (ssize_t)(sizeof(uint64_t) * 2);
#endif
}

static ChValue prim_generate_symbol(ChVM *vm, ChValue *args, int nargs) {
    if (nargs > 1) {
        snprintf(vm->error, sizeof(vm->error), "generate-symbol: expected 0 or 1 arguments");
        return CH_UNDEFINED;
    }

    const char *pretty = "g";
    if (nargs == 1) {
        if (!ch_is_string(args[0])) {
            snprintf(vm->error, sizeof(vm->error), "generate-symbol: expected string");
            return CH_UNDEFINED;
        }
        pretty = ch_as_string(args[0])->data;
    }

    uint64_t rnd[2] = {0, 0};
    if (!read_os_random(rnd)) {
        snprintf(vm->error, sizeof(vm->error), "generate-symbol: OS entropy source unavailable");
        return CH_UNDEFINED;
    }

    uint64_t n = atomic_fetch_add_explicit(&gensym_counter, 1, memory_order_relaxed);
    char buf[512];
    int written = snprintf(buf, sizeof(buf), "%s.%llx.%016llx%016llx", pretty,
                           (unsigned long long)n, (unsigned long long)rnd[0],
                           (unsigned long long)rnd[1]);
    if (written < 0 || (size_t)written >= sizeof(buf)) {
        snprintf(vm->error, sizeof(vm->error), "generate-symbol: name too long");
        return CH_UNDEFINED;
    }
    return ch_gc_intern_symbol(&vm->gc, buf, (size_t)written);
}

void ch_register_srfi260_primitives(ChVM *vm) {
    define_prim(vm, "generate-symbol", prim_generate_symbol, -1, 0);
}
