#include "chaaya/prim.h"

#include "prim_port_internal.h"

#include "chaaya/fiber.h"
#include "chaaya/sandbox.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#if !defined(_WIN32)
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#endif

void define_prim(ChVM *vm, const char *name, ChNativeFn fn, int arity, int min_arity) {
    ChValue sym = ch_gc_intern_symbol_cstr(&vm->gc, name);
    ChSymbol *s = ch_as_symbol(sym);
    int idx = ch_vm_intern_global(vm, s);
    ChValue nv = ch_gc_make_native(&vm->gc, fn, name, arity, min_arity);
    ch_vm_define_global(vm, idx, nv);
}

ChValue get_global(ChVM *vm, const char *name) {
    ChValue sym = ch_gc_intern_symbol_cstr(&vm->gc, name);
    int idx = ch_vm_intern_global(vm, ch_as_symbol(sym));
    return vm->globals[idx].value;
}

static void set_global(ChVM *vm, const char *name, ChValue v) {
    ChValue sym = ch_gc_intern_symbol_cstr(&vm->gc, name);
    int idx = ch_vm_intern_global(vm, ch_as_symbol(sym));
    ch_vm_define_global(vm, idx, v);
}

FILE *port_file(ChPort *p) {
    if (p->kind == CH_PORT_STDIO || p->kind == CH_PORT_FILE) {
        return p->file;
    }
    return NULL;
}

void compact_port_input_buffer(ChPort *p) {
    if (!p->buf || p->pos == 0) {
        return;
    }
    if (p->pos >= p->len) {
        p->pos = 0;
        p->len = 0;
        return;
    }
    size_t unread = p->len - p->pos;
    memmove(p->buf, p->buf + p->pos, unread);
    p->len = unread;
    p->pos = 0;
}

int ensure_port_capacity(ChPort *p, size_t extra) {
    if (extra == 0) {
        return 0;
    }
    size_t need = p->len + extra;
    if (p->kind == CH_PORT_STRING_OUT) {
        need += 1; /* trailing NUL */
    }
    if (need <= p->cap) {
        return 0;
    }
    size_t ncap = p->cap ? p->cap : 64;
    while (ncap < need) {
        ncap *= 2;
    }
    char *nb = (char *)realloc(p->buf, ncap);
    if (!nb) {
        return -1;
    }
    p->buf = nb;
    p->cap = ncap;
    return 0;
}

#if !defined(_WIN32)
/* Lazily put a file port into nonblocking mode once a fiber scheduler parks on it. */
static int ensure_port_nonblocking(ChPort *p) {
    if (!p || p->nonblocking || !p->file) {
        return 0;
    }
    int fd = fileno(p->file);
    if (fd < 0) {
        return -1;
    }
    int flags = fcntl(fd, F_GETFL);
    if (flags < 0) {
        return -1;
    }
    if ((flags & O_NONBLOCK) == 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        return -1;
    }
    p->nonblocking = 1;
    return 0;
}

/* Wake any fiber parked on this port's fd (ONESHOT registration). */
static void wake_port_io_waiters(ChVM *vm, ChPort *p) {
    if (!vm || !vm->fiber_runtime || !p || !p->file) {
        return;
    }
    int fd = fileno(p->file);
    if (fd < 0) {
        return;
    }
    ChReactor *reactor = &vm->fiber_runtime->reactor;
    for (size_t i = 0; i < CH_REACTOR_MAX_FDS; i++) {
        if (!reactor->fds[i].active || reactor->fds[i].fd != fd) {
            continue;
        }
        ChValue payload = reactor->fds[i].payload;
        (void)ch_reactor_unregister_fd(reactor, fd);
        if (ch_is_fiber(payload)) {
            ChFiber *f = ch_as_fiber(payload);
            if (f->state == CH_FIBER_WAITING || f->state == CH_FIBER_IO_WAITING) {
                f->state = CH_FIBER_READY;
                f->io_fd = -1;
                ChFiberRuntime *rt = vm->fiber_runtime;
                if (!f->queued && rt->ready_count < CH_FIBER_READY_MAX) {
                    size_t tail = (rt->ready_head + rt->ready_count) % CH_FIBER_READY_MAX;
                    rt->ready[tail] = payload;
                    rt->ready_count++;
                    f->queued = 1;
                }
            }
        }
        break;
    }
}
#endif

int port_write_bytes_vm(ChVM *vm, ChPort *p, const char *data, size_t len, int may_park) {
    if (p->closed || !p->output) {
        return -1;
    }
    if (p->kind == CH_PORT_STDIO || p->kind == CH_PORT_FILE) {
#if !defined(_WIN32)
        if (may_park && vm && vm->fiber_runtime && ch_is_fiber(vm->fiber_runtime->current) &&
            p->file) {
            int fd = fileno(p->file);
            if (fd >= 0) {
                (void)ensure_port_nonblocking(p);
                size_t written = 0;
                while (written < len) {
                    ssize_t n = write(fd, data + written, len - written);
                    if (n < 0) {
                        if (errno == EINTR) {
                            continue;
                        }
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                            if (ch_fiber_wait_fd(vm, fd, CH_REACTOR_WRITE) != 0) {
                                return -1;
                            }
                            continue; /* fd writable; retry write */
                        }
                        return -1;
                    }
                    if (n == 0) {
                        return -1;
                    }
                    written += (size_t)n;
                }
                return 0;
            }
        }
#else
        (void)vm;
        (void)may_park;
#endif
        return fwrite(data, 1, len, p->file) == len ? 0 : -1;
    }
    if (p->kind == CH_PORT_STRING_OUT) {
        if (ensure_port_capacity(p, len) != 0) {
            return -1;
        }
        memcpy(p->buf + p->len, data, len);
        p->len += len;
        p->buf[p->len] = '\0';
        return 0;
    }
    if (p->kind == CH_PORT_BYTEVECTOR) {
        if (ensure_port_capacity(p, len) != 0) {
            return -1;
        }
        memcpy(p->buf + p->len, data, len);
        p->len += len;
        return 0;
    }
    return -1;
}

int port_write_bytes(ChPort *p, const char *data, size_t len) {
    return port_write_bytes_vm(NULL, p, data, len, 0);
}

int append_file_input_bytes(ChVM *vm, ChPort *p, int may_park) {
    if (p->kind != CH_PORT_STDIO && p->kind != CH_PORT_FILE) {
        return 0;
    }
    if (!p->file || p->closed || !p->input) {
        return -1;
    }
#if !defined(_WIN32)
    int fd = fileno(p->file);
    int in_fiber = may_park && vm && vm->fiber_runtime &&
                   ch_is_fiber(vm->fiber_runtime->current) && fd >= 0;
    if (in_fiber) {
        (void)ensure_port_nonblocking(p);
    }
#else
    (void)vm;
    (void)may_park;
    int in_fiber = 0;
    int fd = -1;
#endif
    size_t free_space = p->cap > p->len ? p->cap - p->len : 0;
    if (free_space == 0) {
        if (ensure_port_capacity(p, CH_PORT_INPUT_CHUNK) != 0) {
            return -1;
        }
        free_space = p->cap - p->len;
    }
    size_t to_read = free_space;
    if (to_read > CH_PORT_INPUT_CHUNK) {
        to_read = CH_PORT_INPUT_CHUNK;
    }
#if !defined(_WIN32)
    if (in_fiber && p->nonblocking) {
        for (;;) {
            ssize_t n = read(fd, p->buf + p->len, to_read);
            if (n < 0) {
                if (errno == EINTR) {
                    continue;
                }
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    if (ch_fiber_wait_fd(vm, fd, CH_REACTOR_READ) != 0) {
                        return -1;
                    }
                    continue; /* fd ready; retry read */
                }
                return -1;
            }
            if (n == 0) {
                return 0;
            }
            p->len += (size_t)n;
            return (int)n;
        }
    }
    if (in_fiber) {
        struct pollfd pfd;
        memset(&pfd, 0, sizeof(pfd));
        pfd.fd = fd;
        pfd.events = POLLIN;
        int pr = poll(&pfd, 1, 0);
        if (pr == 0) {
            if (ch_fiber_wait_fd(vm, fd, CH_REACTOR_READ) != 0) {
                return -1;
            }
        }
    }
#endif
    size_t n = fread(p->buf + p->len, 1, to_read, p->file);
    if (n == 0) {
        return ferror(p->file) ? -1 : 0;
    }
    p->len += n;
    return (int)n;
}

int ensure_port_input_byte(ChVM *vm, ChPort *p, int may_park) {
    if (p->closed || !p->input) {
        return -1;
    }
    if (p->kind == CH_PORT_STRING_IN || p->kind == CH_PORT_BYTEVECTOR) {
        return p->pos < p->len ? 1 : 0;
    }
    if (p->kind == CH_PORT_STDIO || p->kind == CH_PORT_FILE) {
        while (p->pos >= p->len) {
            compact_port_input_buffer(p);
            int appended = append_file_input_bytes(vm, p, may_park);
            if (appended == -2) {
                return -2;
            }
            if (appended <= 0) {
                return appended;
            }
        }
        return 1;
    }
    return -1;
}

int parse_nonnegative_fixnum(ChVM *vm, ChValue v, size_t *out, const char *who) {
    if (!ch_is_fixnum(v)) {
        snprintf(vm->error, sizeof(vm->error), "%s: expected exact integer", who);
        return -1;
    }
    int64_t n = ch_to_fixnum(v);
    if (n < 0) {
        snprintf(vm->error, sizeof(vm->error), "%s: expected non-negative integer", who);
        return -1;
    }
    *out = (size_t)n;
    return 0;
}

int parse_u8(ChVM *vm, ChValue v, uint8_t *out, const char *who) {
    if (!ch_is_fixnum(v)) {
        snprintf(vm->error, sizeof(vm->error), "%s: expected exact integer", who);
        return -1;
    }
    int64_t n = ch_to_fixnum(v);
    if (n < 0 || n > 255) {
        snprintf(vm->error, sizeof(vm->error), "%s: byte out of range", who);
        return -1;
    }
    *out = (uint8_t)n;
    return 0;
}

ChPort *require_output_port(ChVM *vm, ChValue *args, int nargs, int idx) {
    ChValue pv;
    if (nargs > idx) {
        pv = args[idx];
    } else {
        pv = get_global(vm, "current-output-port");
        if (ch_is_procedure(pv)) {
            ChValue out = CH_VOID;
            if (ch_vm_apply(vm, pv, NULL, 0, &out) != CH_VM_OK) {
                return NULL;
            }
            pv = out;
        }
    }
    if (!ch_is_port(pv)) {
        snprintf(vm->error, sizeof(vm->error), "not an output port");
        return NULL;
    }
    ChPort *p = ch_as_port(pv);
    if (!p->output || p->closed) {
        snprintf(vm->error, sizeof(vm->error), "not an open output port");
        return NULL;
    }
    return p;
}

ChPort *require_input_port(ChVM *vm, ChValue *args, int nargs, int idx) {
    ChValue pv;
    if (nargs > idx) {
        pv = args[idx];
    } else {
        pv = get_global(vm, "current-input-port");
        if (ch_is_procedure(pv)) {
            ChValue out = CH_VOID;
            if (ch_vm_apply(vm, pv, NULL, 0, &out) != CH_VM_OK) {
                return NULL;
            }
            pv = out;
        }
    }
    if (!ch_is_port(pv)) {
        snprintf(vm->error, sizeof(vm->error), "not an input port");
        return NULL;
    }
    ChPort *p = ch_as_port(pv);
    if (!p->input || p->closed) {
        snprintf(vm->error, sizeof(vm->error), "not an open input port");
        return NULL;
    }
    return p;
}

static ChValue prim_port_p(ChVM *vm, ChValue *args, int nargs) {
    (void)vm;
    (void)nargs;
    return ch_is_port(args[0]) ? CH_TRUE : CH_FALSE;
}

static ChValue prim_input_port_p(ChVM *vm, ChValue *args, int nargs) {
    (void)vm;
    (void)nargs;
    return (ch_is_port(args[0]) && ch_as_port(args[0])->input) ? CH_TRUE : CH_FALSE;
}

static ChValue prim_output_port_p(ChVM *vm, ChValue *args, int nargs) {
    (void)vm;
    (void)nargs;
    return (ch_is_port(args[0]) && ch_as_port(args[0])->output) ? CH_TRUE : CH_FALSE;
}

static ChValue prim_input_port_open_p(ChVM *vm, ChValue *args, int nargs) {
    (void)vm;
    (void)nargs;
    if (!ch_is_port(args[0])) {
        return CH_FALSE;
    }
    ChPort *p = ch_as_port(args[0]);
    return (p->input && !p->closed) ? CH_TRUE : CH_FALSE;
}

static ChValue prim_output_port_open_p(ChVM *vm, ChValue *args, int nargs) {
    (void)vm;
    (void)nargs;
    if (!ch_is_port(args[0])) {
        return CH_FALSE;
    }
    ChPort *p = ch_as_port(args[0]);
    return (p->output && !p->closed) ? CH_TRUE : CH_FALSE;
}

static ChValue prim_textual_port_p(ChVM *vm, ChValue *args, int nargs) {
    (void)vm;
    (void)nargs;
    if (!ch_is_port(args[0])) {
        return CH_FALSE;
    }
    ChPort *p = ch_as_port(args[0]);
    if (p->kind == CH_PORT_BYTEVECTOR) {
        return CH_FALSE;
    }
    if (p->kind == CH_PORT_FILE) {
        return p->binary ? CH_FALSE : CH_TRUE;
    }
    return CH_TRUE;
}

static ChValue prim_binary_port_p(ChVM *vm, ChValue *args, int nargs) {
    (void)vm;
    (void)nargs;
    if (!ch_is_port(args[0])) {
        return CH_FALSE;
    }
    ChPort *p = ch_as_port(args[0]);
    if (p->kind == CH_PORT_BYTEVECTOR) {
        return CH_TRUE;
    }
    if (p->kind == CH_PORT_FILE) {
        return p->binary ? CH_TRUE : CH_FALSE;
    }
    return CH_FALSE;
}

static ChValue prim_eof_object_p(ChVM *vm, ChValue *args, int nargs) {
    (void)vm;
    (void)nargs;
    return args[0] == CH_EOF_OBJ ? CH_TRUE : CH_FALSE;
}

static ChValue prim_eof_object(ChVM *vm, ChValue *args, int nargs) {
    (void)vm;
    (void)args;
    (void)nargs;
    return CH_EOF_OBJ;
}

static ChValue prim_current_input_port(ChVM *vm, ChValue *args, int nargs) {
    (void)args;
    (void)nargs;
    return get_global(vm, "%current-input-port");
}

static ChValue prim_current_output_port(ChVM *vm, ChValue *args, int nargs) {
    (void)args;
    (void)nargs;
    return get_global(vm, "%current-output-port");
}

static ChValue prim_current_error_port(ChVM *vm, ChValue *args, int nargs) {
    (void)args;
    (void)nargs;
    return get_global(vm, "%current-error-port");
}

static ChValue prim_open_input_string(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    if (!ch_is_string(args[0])) {
        snprintf(vm->error, sizeof(vm->error), "open-input-string: not a string");
        return CH_UNDEFINED;
    }
    ChString *s = ch_as_string(args[0]);
    return ch_gc_make_string_input_port(&vm->gc, s->data, s->len);
}

static ChValue prim_open_output_string(ChVM *vm, ChValue *args, int nargs) {
    (void)args;
    (void)nargs;
    return ch_gc_make_string_output_port(&vm->gc);
}

static ChValue prim_get_output_string(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    if (!ch_is_port(args[0]) || ch_as_port(args[0])->kind != CH_PORT_STRING_OUT) {
        snprintf(vm->error, sizeof(vm->error), "get-output-string: not a string output port");
        return CH_UNDEFINED;
    }
    ChPort *p = ch_as_port(args[0]);
    return ch_gc_make_string(&vm->gc, p->buf ? p->buf : "", p->len);
}

static ChValue prim_open_input_bytevector(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    if (!ch_is_bytevector(args[0])) {
        snprintf(vm->error, sizeof(vm->error), "open-input-bytevector: not a bytevector");
        return CH_UNDEFINED;
    }
    ChBytevector *bv = ch_as_bytevector(args[0]);
    return ch_gc_make_bytevector_input_port(&vm->gc, bv->data, bv->len);
}

static ChValue prim_open_output_bytevector(ChVM *vm, ChValue *args, int nargs) {
    (void)args;
    (void)nargs;
    return ch_gc_make_bytevector_output_port(&vm->gc);
}

static ChValue prim_get_output_bytevector(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    if (!ch_is_port(args[0])) {
        snprintf(vm->error, sizeof(vm->error), "get-output-bytevector: not a port");
        return CH_UNDEFINED;
    }
    ChPort *p = ch_as_port(args[0]);
    if (p->kind != CH_PORT_BYTEVECTOR || !p->output) {
        snprintf(vm->error, sizeof(vm->error), "get-output-bytevector: not a bytevector output port");
        return CH_UNDEFINED;
    }
    ChValue out = ch_gc_make_bytevector(&vm->gc, p->len, 0);
    ChBytevector *bv = ch_as_bytevector(out);
    if (p->len > 0) {
        memcpy(bv->data, p->buf, p->len);
    }
    return out;
}

void close_port_impl(ChVM *vm, ChPort *p) {
    if (p->closed) {
        return;
    }
#if !defined(_WIN32)
    wake_port_io_waiters(vm, p);
#else
    (void)vm;
#endif
    if (p->kind == CH_PORT_FILE && p->file) {
        fclose(p->file);
        p->file = NULL;
    }
    p->closed = 1;
}

static ChValue prim_close_port(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    if (!ch_is_port(args[0])) {
        snprintf(vm->error, sizeof(vm->error), "close-port: not a port");
        return CH_UNDEFINED;
    }
    close_port_impl(vm, ch_as_port(args[0]));
    return CH_VOID;
}

static ChValue prim_flush_output_port(ChVM *vm, ChValue *args, int nargs) {
    if (nargs > 1) {
        snprintf(vm->error, sizeof(vm->error), "flush-output-port: expected 0 or 1 arguments");
        return CH_UNDEFINED;
    }
    ChPort *p = require_output_port(vm, args, nargs, 0);
    if (!p) {
        return CH_UNDEFINED;
    }
    FILE *f = port_file(p);
    if (f && fflush(f) != 0) {
        snprintf(vm->error, sizeof(vm->error), "flush-output-port: flush failed");
        return CH_UNDEFINED;
    }
    return CH_VOID;
}

static const char *require_path(ChVM *vm, ChValue v, const char *who) {
    if (!ch_is_string(v)) {
        snprintf(vm->error, sizeof(vm->error), "%s: expected string path", who);
        return NULL;
    }
    const char *path = ch_as_string(v)->data;
    if (ch_sandbox_deny_fs(path)) {
        snprintf(vm->error, sizeof(vm->error),
                 "%s: denied by sandbox for path '%s'", who, path);
        return NULL;
    }
    return path;
}

ChValue raise_typed_error(ChVM *vm, const char *message, int error_type, ChValue irritant) {
    ChValue msg = ch_gc_make_string(&vm->gc, message, strlen(message));
    ChValue irritants = CH_NIL;
    ch_gc_push(&vm->gc, &msg);
    ch_gc_push(&vm->gc, &irritants);
    if (irritant != CH_UNDEFINED) {
        ChValue item = irritant;
        ch_gc_push(&vm->gc, &item);
        irritants = ch_gc_cons(&vm->gc, item, CH_NIL);
        ch_gc_pop(&vm->gc);
    }
    ChValue err = ch_gc_make_error_object(&vm->gc, msg, irritants, error_type);
    ch_gc_pop_n(&vm->gc, 2);
    return ch_vm_raise(vm, err, 0);
}

ChValue raise_file_error(ChVM *vm, const char *message, ChValue irritant) {
    return raise_typed_error(vm, message, 1, irritant);
}

ChValue raise_read_error(ChVM *vm, const char *message, ChValue irritant) {
    return raise_typed_error(vm, message, 2, irritant);
}

static ChValue prim_open_input_file(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    const char *path = require_path(vm, args[0], "open-input-file");
    if (!path) {
        return CH_UNDEFINED;
    }
    FILE *f = fopen(path, "r");
    if (!f) {
        return raise_file_error(vm, "open-input-file: cannot open file", args[0]);
    }
    return ch_gc_make_file_port(&vm->gc, f, 1, 0, 0);
}

static ChValue prim_open_output_file(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    const char *path = require_path(vm, args[0], "open-output-file");
    if (!path) {
        return CH_UNDEFINED;
    }
    FILE *f = fopen(path, "w");
    if (!f) {
        return raise_file_error(vm, "open-output-file: cannot open file", args[0]);
    }
    return ch_gc_make_file_port(&vm->gc, f, 0, 1, 0);
}

static ChValue prim_open_binary_input_file(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    const char *path = require_path(vm, args[0], "open-binary-input-file");
    if (!path) {
        return CH_UNDEFINED;
    }
    FILE *f = fopen(path, "rb");
    if (!f) {
        return raise_file_error(vm, "open-binary-input-file: cannot open file", args[0]);
    }
    return ch_gc_make_file_port(&vm->gc, f, 1, 0, 1);
}

static ChValue prim_open_binary_output_file(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    const char *path = require_path(vm, args[0], "open-binary-output-file");
    if (!path) {
        return CH_UNDEFINED;
    }
    FILE *f = fopen(path, "wb");
    if (!f) {
        return raise_file_error(vm, "open-binary-output-file: cannot open file", args[0]);
    }
    return ch_gc_make_file_port(&vm->gc, f, 0, 1, 1);
}

static ChValue prim_file_exists_p(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    const char *path = require_path(vm, args[0], "file-exists?");
    if (!path) {
        return CH_UNDEFINED;
    }
    struct stat st;
    /* R7RS: any filesystem object that exists, including directories. */
    return (stat(path, &st) == 0) ? CH_TRUE : CH_FALSE;
}

static ChValue prim_delete_file(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    const char *path = require_path(vm, args[0], "delete-file");
    if (!path) {
        return CH_UNDEFINED;
    }
    if (remove(path) != 0) {
        return raise_file_error(vm, "delete-file: cannot delete file", args[0]);
    }
    return CH_VOID;
}

static ChValue prim_call_with_input_file(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    ChValue port = prim_open_input_file(vm, args, 1);
    if (port == CH_UNDEFINED) {
        return CH_UNDEFINED;
    }
    if (!ch_is_procedure(args[1])) {
        close_port_impl(vm, ch_as_port(port));
        snprintf(vm->error, sizeof(vm->error), "call-with-input-file: not a procedure");
        return CH_UNDEFINED;
    }
    ChValue result = CH_UNDEFINED;
    ch_gc_push(&vm->gc, &port);
    ch_gc_push(&vm->gc, &result);
    if (ch_vm_apply(vm, args[1], &port, 1, &result) != CH_VM_OK) {
        close_port_impl(vm, ch_as_port(port));
        ch_gc_pop_n(&vm->gc, 2);
        return CH_UNDEFINED;
    }
    close_port_impl(vm, ch_as_port(port));
    ch_gc_pop_n(&vm->gc, 2);
    return result;
}

static ChValue prim_call_with_output_file(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    ChValue port = prim_open_output_file(vm, args, 1);
    if (port == CH_UNDEFINED) {
        return CH_UNDEFINED;
    }
    if (!ch_is_procedure(args[1])) {
        close_port_impl(vm, ch_as_port(port));
        snprintf(vm->error, sizeof(vm->error), "call-with-output-file: not a procedure");
        return CH_UNDEFINED;
    }
    ChValue result = CH_UNDEFINED;
    ch_gc_push(&vm->gc, &port);
    ch_gc_push(&vm->gc, &result);
    if (ch_vm_apply(vm, args[1], &port, 1, &result) != CH_VM_OK) {
        close_port_impl(vm, ch_as_port(port));
        ch_gc_pop_n(&vm->gc, 2);
        return CH_UNDEFINED;
    }
    close_port_impl(vm, ch_as_port(port));
    ch_gc_pop_n(&vm->gc, 2);
    return result;
}

static ChValue prim_call_with_port(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    if (!ch_is_port(args[0])) {
        snprintf(vm->error, sizeof(vm->error), "call-with-port: not a port");
        return CH_UNDEFINED;
    }
    if (!ch_is_procedure(args[1])) {
        close_port_impl(vm, ch_as_port(args[0]));
        snprintf(vm->error, sizeof(vm->error), "call-with-port: not a procedure");
        return CH_UNDEFINED;
    }
    ChValue port = args[0];
    ChValue result = CH_UNDEFINED;
    ch_gc_push(&vm->gc, &port);
    ch_gc_push(&vm->gc, &result);
    ChVMStatus st = ch_vm_apply(vm, args[1], &port, 1, &result);
    close_port_impl(vm, ch_as_port(port));
    ch_gc_pop_n(&vm->gc, 2);
    if (st == CH_VM_CONTINUATION_INVOKED) {
        vm->continuation_invoked = true;
        return CH_UNDEFINED;
    }
    if (st != CH_VM_OK) {
        return CH_UNDEFINED;
    }
    return result;
}

static ChValue prim_with_input_from_file(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    ChValue port = prim_open_input_file(vm, args, 1);
    if (port == CH_UNDEFINED) {
        return CH_UNDEFINED;
    }
    if (!ch_is_procedure(args[1])) {
        close_port_impl(vm, ch_as_port(port));
        snprintf(vm->error, sizeof(vm->error), "with-input-from-file: not a procedure");
        return CH_UNDEFINED;
    }
    ChValue parameter = get_global(vm, "current-input-port");
    if (!ch_is_parameter(parameter)) {
        close_port_impl(vm, ch_as_port(port));
        snprintf(vm->error, sizeof(vm->error), "with-input-from-file: current-input-port not a parameter");
        return CH_UNDEFINED;
    }
    if (ch_vm_parameter_push(vm, parameter, port) != 0) {
        close_port_impl(vm, ch_as_port(port));
        return CH_UNDEFINED;
    }
    ChValue result = CH_UNDEFINED;
    ch_gc_push(&vm->gc, &port);
    ch_gc_push(&vm->gc, &parameter);
    ch_gc_push(&vm->gc, &result);
    ChVMStatus st = ch_vm_apply(vm, args[1], NULL, 0, &result);
    int pop_rc = ch_vm_parameter_pop(vm, parameter);
    close_port_impl(vm, ch_as_port(port));
    ch_gc_pop_n(&vm->gc, 3);
    if (pop_rc != 0) {
        return CH_UNDEFINED;
    }
    if (st != CH_VM_OK) {
        return CH_UNDEFINED;
    }
    return result;
}

static ChValue prim_with_output_to_file(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    ChValue port = prim_open_output_file(vm, args, 1);
    if (port == CH_UNDEFINED) {
        return CH_UNDEFINED;
    }
    if (!ch_is_procedure(args[1])) {
        close_port_impl(vm, ch_as_port(port));
        snprintf(vm->error, sizeof(vm->error), "with-output-to-file: not a procedure");
        return CH_UNDEFINED;
    }
    ChValue parameter = get_global(vm, "current-output-port");
    if (!ch_is_parameter(parameter)) {
        close_port_impl(vm, ch_as_port(port));
        snprintf(vm->error, sizeof(vm->error),
                 "with-output-to-file: current-output-port not a parameter");
        return CH_UNDEFINED;
    }
    if (ch_vm_parameter_push(vm, parameter, port) != 0) {
        close_port_impl(vm, ch_as_port(port));
        return CH_UNDEFINED;
    }
    ChValue result = CH_UNDEFINED;
    ch_gc_push(&vm->gc, &port);
    ch_gc_push(&vm->gc, &parameter);
    ch_gc_push(&vm->gc, &result);
    ChVMStatus st = ch_vm_apply(vm, args[1], NULL, 0, &result);
    int pop_rc = ch_vm_parameter_pop(vm, parameter);
    close_port_impl(vm, ch_as_port(port));
    ch_gc_pop_n(&vm->gc, 3);
    if (pop_rc != 0) {
        return CH_UNDEFINED;
    }
    if (st != CH_VM_OK) {
        return CH_UNDEFINED;
    }
    return result;
}

void ch_register_port_primitives(ChVM *vm) {
    ChValue in = ch_gc_make_stdio_port(&vm->gc, stdin, 1, 0);
    ChValue out = ch_gc_make_stdio_port(&vm->gc, stdout, 0, 1);
    ChValue err = ch_gc_make_stdio_port(&vm->gc, stderr, 0, 1);
    ch_gc_push(&vm->gc, &in);
    ch_gc_push(&vm->gc, &out);
    ch_gc_push(&vm->gc, &err);
    ChValue in_param = ch_gc_make_parameter(&vm->gc, in, CH_NIL);
    ChValue out_param = ch_gc_make_parameter(&vm->gc, out, CH_NIL);
    ChValue err_param = ch_gc_make_parameter(&vm->gc, err, CH_NIL);
    ch_gc_push(&vm->gc, &in_param);
    ch_gc_push(&vm->gc, &out_param);
    ch_gc_push(&vm->gc, &err_param);

    define_prim(vm, "port?", prim_port_p, 1, 1);
    define_prim(vm, "input-port?", prim_input_port_p, 1, 1);
    define_prim(vm, "output-port?", prim_output_port_p, 1, 1);
    define_prim(vm, "input-port-open?", prim_input_port_open_p, 1, 1);
    define_prim(vm, "output-port-open?", prim_output_port_open_p, 1, 1);
    define_prim(vm, "textual-port?", prim_textual_port_p, 1, 1);
    define_prim(vm, "binary-port?", prim_binary_port_p, 1, 1);
    define_prim(vm, "eof-object?", prim_eof_object_p, 1, 1);
    define_prim(vm, "eof-object", prim_eof_object, 0, 0);
    define_prim(vm, "current-input-port", prim_current_input_port, 0, 0);
    define_prim(vm, "current-output-port", prim_current_output_port, 0, 0);
    define_prim(vm, "current-error-port", prim_current_error_port, 0, 0);
    define_prim(vm, "open-input-string", prim_open_input_string, 1, 1);
    define_prim(vm, "open-output-string", prim_open_output_string, 0, 0);
    define_prim(vm, "get-output-string", prim_get_output_string, 1, 1);
    define_prim(vm, "open-input-bytevector", prim_open_input_bytevector, 1, 1);
    define_prim(vm, "open-output-bytevector", prim_open_output_bytevector, 0, 0);
    define_prim(vm, "get-output-bytevector", prim_get_output_bytevector, 1, 1);
    define_prim(vm, "close-port", prim_close_port, 1, 1);
    define_prim(vm, "close-input-port", prim_close_port, 1, 1);
    define_prim(vm, "close-output-port", prim_close_port, 1, 1);
    define_prim(vm, "flush-output-port", prim_flush_output_port, -1, 0);
    define_prim(vm, "open-input-file", prim_open_input_file, 1, 1);
    define_prim(vm, "open-output-file", prim_open_output_file, 1, 1);
    define_prim(vm, "open-binary-input-file", prim_open_binary_input_file, 1, 1);
    define_prim(vm, "open-binary-output-file", prim_open_binary_output_file, 1, 1);
    define_prim(vm, "file-exists?", prim_file_exists_p, 1, 1);
    define_prim(vm, "delete-file", prim_delete_file, 1, 1);
    define_prim(vm, "call-with-port", prim_call_with_port, 2, 2);
    define_prim(vm, "call-with-input-file", prim_call_with_input_file, 2, 2);
    define_prim(vm, "call-with-output-file", prim_call_with_output_file, 2, 2);
    define_prim(vm, "with-input-from-file", prim_with_input_from_file, 2, 2);
    define_prim(vm, "with-output-to-file", prim_with_output_to_file, 2, 2);

    /* char/string/u8/bytevector I/O, display/write family, read, port-position. */
    ch_register_port_io_primitives(vm);

    /* Current ports are parameter objects (R7RS dynamic binding). */
    set_global(vm, "current-input-port", in_param);
    set_global(vm, "current-output-port", out_param);
    set_global(vm, "current-error-port", err_param);
    set_global(vm, "%current-input-port", in_param);
    set_global(vm, "%current-output-port", out_param);
    set_global(vm, "%current-error-port", err_param);
    ch_gc_pop_n(&vm->gc, 6);
}
