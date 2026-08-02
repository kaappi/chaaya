/* Milestone 2: fiber waiting on pipe read must not freeze a sibling. */
#include "test_helpers.h"

#include "chaaya/fiber.h"

#include <stdio.h>
#include <unistd.h>

#if defined(_WIN32)

int main(void) {
    printf("skip fiber io on Windows\n");
    return 0;
}

#else

static int g_pipe_fds[2] = {-1, -1};
static int g_sibling_ran = 0;
static int g_reader_done = 0;

static ChValue prim_park_read(ChVM *vm, ChValue *args, int nargs) {
    (void)args;
    (void)nargs;
    if (ch_fiber_wait_fd(vm, g_pipe_fds[0], CH_REACTOR_READ) != 0) {
        fprintf(stderr, "wait_fd failed: %s\n", vm->error);
        return CH_UNDEFINED;
    }
    char buf[8];
    ssize_t n = read(g_pipe_fds[0], buf, sizeof(buf));
    g_reader_done = 1;
    return ch_make_fixnum((int64_t)n);
}

static ChValue prim_sibling_mark(ChVM *vm, ChValue *args, int nargs) {
    (void)vm;
    (void)args;
    (void)nargs;
    g_sibling_ran = 1;
    /* Unblock the reader after proving the sibling ran. */
    (void)write(g_pipe_fds[1], "x", 1);
    return CH_TRUE;
}

static void define_prim(ChVM *vm, const char *name, ChNativeFn fn, int arity) {
    ChValue sym = ch_gc_intern_symbol_cstr(&vm->gc, name);
    int idx = ch_vm_intern_global(vm, ch_as_symbol(sym));
    ChValue nv = ch_gc_make_native(&vm->gc, fn, name, arity, arity);
    ch_vm_define_global(vm, idx, nv);
}

int main(void) {
    if (pipe(g_pipe_fds) != 0) {
        fprintf(stderr, "pipe failed\n");
        return 1;
    }

    ChVM vm;
    ch_vm_init(&vm);
    ch_vm_register_primitives(&vm);
    define_prim(&vm, "%park-read", prim_park_read, 0);
    define_prim(&vm, "%sibling-mark", prim_sibling_mark, 0);

    ChValue out = CH_UNDEFINED;
    if (!ch_test_eval_all(&vm,
                          "(define reader (spawn (lambda () (%park-read))))\n"
                          "(define sib (spawn (lambda () (%sibling-mark))))\n"
                          "(fiber-join reader)\n"
                          "(fiber-join sib)\n",
                          &out)) {
        fprintf(stderr, "eval failed: %s\n", vm.error);
        close(g_pipe_fds[0]);
        close(g_pipe_fds[1]);
        return 1;
    }

    if (!g_sibling_ran || !g_reader_done) {
        fprintf(stderr, "expected sibling_ran=%d reader_done=%d\n", g_sibling_ran,
                g_reader_done);
        close(g_pipe_fds[0]);
        close(g_pipe_fds[1]);
        return 1;
    }

    close(g_pipe_fds[0]);
    close(g_pipe_fds[1]);
    printf("fiber io pipe sibling ok\n");
    return 0;
}

#endif
