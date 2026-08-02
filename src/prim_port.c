#include "chaaya/prim.h"

#include "chaaya/fiber.h"
#include "chaaya/printer.h"
#include "chaaya/reader.h"
#include "chaaya/sandbox.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#if !defined(_WIN32)
#include <poll.h>
#include <unistd.h>
#endif

#define CH_PORT_INPUT_CHUNK 4096

static void define_prim(ChVM *vm, const char *name, ChNativeFn fn, int arity, int min_arity) {
    ChValue sym = ch_gc_intern_symbol_cstr(&vm->gc, name);
    ChSymbol *s = ch_as_symbol(sym);
    int idx = ch_vm_intern_global(vm, s);
    ChValue nv = ch_gc_make_native(&vm->gc, fn, name, arity, min_arity);
    ch_vm_define_global(vm, idx, nv);
}

static ChValue get_global(ChVM *vm, const char *name) {
    ChValue sym = ch_gc_intern_symbol_cstr(&vm->gc, name);
    int idx = ch_vm_intern_global(vm, ch_as_symbol(sym));
    return vm->globals[idx].value;
}

static void set_global(ChVM *vm, const char *name, ChValue v) {
    ChValue sym = ch_gc_intern_symbol_cstr(&vm->gc, name);
    int idx = ch_vm_intern_global(vm, ch_as_symbol(sym));
    ch_vm_define_global(vm, idx, v);
}

static FILE *port_file(ChPort *p) {
    if (p->kind == CH_PORT_STDIO || p->kind == CH_PORT_FILE) {
        return p->file;
    }
    return NULL;
}

static void compact_port_input_buffer(ChPort *p) {
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

static int ensure_port_capacity(ChPort *p, size_t extra) {
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

static int port_write_bytes(ChPort *p, const char *data, size_t len) {
    if (p->closed || !p->output) {
        return -1;
    }
    if (p->kind == CH_PORT_STDIO || p->kind == CH_PORT_FILE) {
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

/* Returns bytes appended (>0), 0 at EOF, -1 on error, -2 if fiber parked on fd. */
static int append_file_input_bytes(ChVM *vm, ChPort *p, int may_park) {
    if (p->kind != CH_PORT_STDIO && p->kind != CH_PORT_FILE) {
        return 0;
    }
    if (!p->file || p->closed || !p->input) {
        return -1;
    }
#if !defined(_WIN32)
    int fd = fileno(p->file);
    if (may_park && vm && vm->fiber_runtime && ch_is_fiber(vm->fiber_runtime->current) &&
        fd >= 0) {
        struct pollfd pfd;
        memset(&pfd, 0, sizeof(pfd));
        pfd.fd = fd;
        pfd.events = POLLIN;
        int pr = poll(&pfd, 1, 0);
        if (pr == 0) {
            if (ch_fiber_wait_fd(vm, fd, CH_REACTOR_READ) != 0) {
                return -1;
            }
            return -2;
        }
    }
#else
    (void)vm;
    (void)may_park;
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
    size_t n = fread(p->buf + p->len, 1, to_read, p->file);
    if (n == 0) {
        return ferror(p->file) ? -1 : 0;
    }
    p->len += n;
    return (int)n;
}

static int ensure_port_input_byte(ChVM *vm, ChPort *p, int may_park) {
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

static int parse_nonnegative_fixnum(ChVM *vm, ChValue v, size_t *out, const char *who) {
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

static int parse_u8(ChVM *vm, ChValue v, uint8_t *out, const char *who) {
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

static ChPort *require_output_port(ChVM *vm, ChValue *args, int nargs, int idx) {
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

static ChPort *require_input_port(ChVM *vm, ChValue *args, int nargs, int idx) {
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

static void close_port_impl(ChPort *p) {
    if (p->closed) {
        return;
    }
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
    close_port_impl(ch_as_port(args[0]));
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

static ChValue raise_typed_error(ChVM *vm, const char *message, int error_type, ChValue irritant) {
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

static ChValue raise_file_error(ChVM *vm, const char *message, ChValue irritant) {
    return raise_typed_error(vm, message, 1, irritant);
}

static ChValue raise_read_error(ChVM *vm, const char *message, ChValue irritant) {
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
        close_port_impl(ch_as_port(port));
        snprintf(vm->error, sizeof(vm->error), "call-with-input-file: not a procedure");
        return CH_UNDEFINED;
    }
    ChValue result = CH_UNDEFINED;
    ch_gc_push(&vm->gc, &port);
    ch_gc_push(&vm->gc, &result);
    if (ch_vm_apply(vm, args[1], &port, 1, &result) != CH_VM_OK) {
        close_port_impl(ch_as_port(port));
        ch_gc_pop_n(&vm->gc, 2);
        return CH_UNDEFINED;
    }
    close_port_impl(ch_as_port(port));
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
        close_port_impl(ch_as_port(port));
        snprintf(vm->error, sizeof(vm->error), "call-with-output-file: not a procedure");
        return CH_UNDEFINED;
    }
    ChValue result = CH_UNDEFINED;
    ch_gc_push(&vm->gc, &port);
    ch_gc_push(&vm->gc, &result);
    if (ch_vm_apply(vm, args[1], &port, 1, &result) != CH_VM_OK) {
        close_port_impl(ch_as_port(port));
        ch_gc_pop_n(&vm->gc, 2);
        return CH_UNDEFINED;
    }
    close_port_impl(ch_as_port(port));
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
        close_port_impl(ch_as_port(args[0]));
        snprintf(vm->error, sizeof(vm->error), "call-with-port: not a procedure");
        return CH_UNDEFINED;
    }
    ChValue port = args[0];
    ChValue result = CH_UNDEFINED;
    ch_gc_push(&vm->gc, &port);
    ch_gc_push(&vm->gc, &result);
    ChVMStatus st = ch_vm_apply(vm, args[1], &port, 1, &result);
    close_port_impl(ch_as_port(port));
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
        close_port_impl(ch_as_port(port));
        snprintf(vm->error, sizeof(vm->error), "with-input-from-file: not a procedure");
        return CH_UNDEFINED;
    }
    ChValue parameter = get_global(vm, "current-input-port");
    if (!ch_is_parameter(parameter)) {
        close_port_impl(ch_as_port(port));
        snprintf(vm->error, sizeof(vm->error), "with-input-from-file: current-input-port not a parameter");
        return CH_UNDEFINED;
    }
    if (ch_vm_parameter_push(vm, parameter, port) != 0) {
        close_port_impl(ch_as_port(port));
        return CH_UNDEFINED;
    }
    ChValue result = CH_UNDEFINED;
    ch_gc_push(&vm->gc, &port);
    ch_gc_push(&vm->gc, &parameter);
    ch_gc_push(&vm->gc, &result);
    ChVMStatus st = ch_vm_apply(vm, args[1], NULL, 0, &result);
    int pop_rc = ch_vm_parameter_pop(vm, parameter);
    close_port_impl(ch_as_port(port));
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
        close_port_impl(ch_as_port(port));
        snprintf(vm->error, sizeof(vm->error), "with-output-to-file: not a procedure");
        return CH_UNDEFINED;
    }
    ChValue parameter = get_global(vm, "current-output-port");
    if (!ch_is_parameter(parameter)) {
        close_port_impl(ch_as_port(port));
        snprintf(vm->error, sizeof(vm->error),
                 "with-output-to-file: current-output-port not a parameter");
        return CH_UNDEFINED;
    }
    if (ch_vm_parameter_push(vm, parameter, port) != 0) {
        close_port_impl(ch_as_port(port));
        return CH_UNDEFINED;
    }
    ChValue result = CH_UNDEFINED;
    ch_gc_push(&vm->gc, &port);
    ch_gc_push(&vm->gc, &parameter);
    ch_gc_push(&vm->gc, &result);
    ChVMStatus st = ch_vm_apply(vm, args[1], NULL, 0, &result);
    int pop_rc = ch_vm_parameter_pop(vm, parameter);
    close_port_impl(ch_as_port(port));
    ch_gc_pop_n(&vm->gc, 3);
    if (pop_rc != 0) {
        return CH_UNDEFINED;
    }
    if (st != CH_VM_OK) {
        return CH_UNDEFINED;
    }
    return result;
}

static void print_to_port(ChPort *p, ChValue v, ChPrintMode mode) {
    FILE *f = port_file(p);
    if (f) {
        ch_print_value_mode(f, v, mode);
        return;
    }
    char *s = ch_value_to_string_mode(v, mode);
    if (s) {
        port_write_bytes(p, s, strlen(s));
        free(s);
    }
}

static ChValue prim_display(ChVM *vm, ChValue *args, int nargs) {
    ChPort *p = require_output_port(vm, args, nargs, 1);
    if (!p) {
        return CH_UNDEFINED;
    }
    print_to_port(p, args[0], CH_PRINT_DISPLAY);
    return CH_VOID;
}

static ChValue prim_write(ChVM *vm, ChValue *args, int nargs) {
    ChPort *p = require_output_port(vm, args, nargs, 1);
    if (!p) {
        return CH_UNDEFINED;
    }
    print_to_port(p, args[0], CH_PRINT_WRITE);
    return CH_VOID;
}

static ChValue prim_write_shared(ChVM *vm, ChValue *args, int nargs) {
    ChPort *p = require_output_port(vm, args, nargs, 1);
    if (!p) {
        return CH_UNDEFINED;
    }
    print_to_port(p, args[0], CH_PRINT_SHARED);
    return CH_VOID;
}

static ChValue prim_write_simple(ChVM *vm, ChValue *args, int nargs) {
    ChPort *p = require_output_port(vm, args, nargs, 1);
    if (!p) {
        return CH_UNDEFINED;
    }
    print_to_port(p, args[0], CH_PRINT_SIMPLE);
    return CH_VOID;
}

static ChValue prim_newline(ChVM *vm, ChValue *args, int nargs) {
    ChPort *p = require_output_port(vm, args, nargs, 0);
    if (!p) {
        return CH_UNDEFINED;
    }
    port_write_bytes(p, "\n", 1);
    return CH_VOID;
}

static int port_peek_byte(ChVM *vm, ChPort *p) {
    int ready = ensure_port_input_byte(vm, p, 1);
    if (ready == -2) {
        return -2;
    }
    if (ready <= 0) {
        return -1;
    }
    return (unsigned char)p->buf[p->pos];
}

static int port_read_byte(ChVM *vm, ChPort *p) {
    int ready = ensure_port_input_byte(vm, p, 1);
    if (ready == -2) {
        return -2;
    }
    if (ready <= 0) {
        return -1;
    }
    return (unsigned char)p->buf[p->pos++];
}

static int utf8_seq_len(unsigned char b0) {
    if (b0 < 0x80) {
        return 1;
    }
    if ((b0 & 0xE0) == 0xC0) {
        return 2;
    }
    if ((b0 & 0xF0) == 0xE0) {
        return 3;
    }
    if ((b0 & 0xF8) == 0xF0) {
        return 4;
    }
    return -1;
}

static int port_decode_utf8_at(ChVM *vm, ChPort *p, size_t pos, uint32_t *cp_out, size_t *next_out) {
    int ready0 = ensure_port_input_byte(vm, p, 1);
    if (ready0 == -2) {
        return -2;
    }
    if (ready0 <= 0 && pos >= p->len) {
        return -1;
    }
    if (pos >= p->len) {
        return -1;
    }
    unsigned char b0 = (unsigned char)p->buf[pos];
    int n = utf8_seq_len(b0);
    if (n < 0) {
        return -1;
    }
    while (pos + (size_t)n > p->len) {
        int ready = ensure_port_input_byte(vm, p, 1);
        if (ready == -2) {
            return -2;
        }
        if (ready < 0) {
            return -1;
        }
        if (ready == 0) {
            break;
        }
    }
    if (pos + (size_t)n > p->len) {
        return -1;
    }
    uint32_t cp = 0;
    if (n == 1) {
        cp = b0;
    } else if (n == 2) {
        cp = ((uint32_t)(b0 & 0x1F) << 6) | ((unsigned char)p->buf[pos + 1] & 0x3F);
    } else if (n == 3) {
        cp = ((uint32_t)(b0 & 0x0F) << 12) | (((unsigned char)p->buf[pos + 1] & 0x3F) << 6) |
             ((unsigned char)p->buf[pos + 2] & 0x3F);
    } else {
        cp = ((uint32_t)(b0 & 0x07) << 18) | (((unsigned char)p->buf[pos + 1] & 0x3F) << 12) |
             (((unsigned char)p->buf[pos + 2] & 0x3F) << 6) | ((unsigned char)p->buf[pos + 3] & 0x3F);
    }
    *cp_out = cp;
    *next_out = pos + (size_t)n;
    return 0;
}

static ChValue prim_read_char(ChVM *vm, ChValue *args, int nargs) {
    ChPort *p = require_input_port(vm, args, nargs, 0);
    if (!p) {
        return CH_UNDEFINED;
    }
    uint32_t cp = 0;
    size_t next = 0;
    int rc = port_decode_utf8_at(vm, p, p->pos, &cp, &next);
    if (rc == -2) {
        return CH_UNDEFINED;
    }
    if (rc != 0) {
        return CH_EOF_OBJ;
    }
    p->pos = next;
    return ch_make_char(cp);
}

static ChValue prim_peek_char(ChVM *vm, ChValue *args, int nargs) {
    ChPort *p = require_input_port(vm, args, nargs, 0);
    if (!p) {
        return CH_UNDEFINED;
    }
    uint32_t cp = 0;
    size_t next = 0;
    int rc = port_decode_utf8_at(vm, p, p->pos, &cp, &next);
    if (rc == -2) {
        return CH_UNDEFINED;
    }
    if (rc != 0) {
        return CH_EOF_OBJ;
    }
    return ch_make_char(cp);
}

static ChValue prim_char_ready_p(ChVM *vm, ChValue *args, int nargs) {
    ChPort *p = require_input_port(vm, args, nargs, 0);
    if (!p) {
        return CH_UNDEFINED;
    }
    int ready = ensure_port_input_byte(vm, p, 0);
    if (ready < 0) {
        return CH_UNDEFINED;
    }
    return ready > 0 ? CH_TRUE : CH_FALSE;
}

static ChValue prim_write_char(ChVM *vm, ChValue *args, int nargs) {
    if (nargs < 1 || !ch_is_char(args[0])) {
        snprintf(vm->error, sizeof(vm->error), "write-char: expected char");
        return CH_UNDEFINED;
    }
    ChPort *p = require_output_port(vm, args, nargs, 1);
    if (!p) {
        return CH_UNDEFINED;
    }
    uint32_t cp = ch_to_char(args[0]);
    char encoded[4];
    size_t n = 0;
    if (cp <= 0x7Fu) {
        encoded[0] = (char)cp;
        n = 1;
    } else if (cp <= 0x7FFu) {
        encoded[0] = (char)(0xC0u | (cp >> 6));
        encoded[1] = (char)(0x80u | (cp & 0x3Fu));
        n = 2;
    } else if (cp <= 0xFFFFu) {
        encoded[0] = (char)(0xE0u | (cp >> 12));
        encoded[1] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        encoded[2] = (char)(0x80u | (cp & 0x3Fu));
        n = 3;
    } else if (cp <= 0x10FFFFu) {
        encoded[0] = (char)(0xF0u | (cp >> 18));
        encoded[1] = (char)(0x80u | ((cp >> 12) & 0x3Fu));
        encoded[2] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        encoded[3] = (char)(0x80u | (cp & 0x3Fu));
        n = 4;
    } else {
        snprintf(vm->error, sizeof(vm->error), "write-char: invalid character");
        return CH_UNDEFINED;
    }
    if (port_write_bytes(p, encoded, n) != 0) {
        snprintf(vm->error, sizeof(vm->error), "write-char: write failed");
        return CH_UNDEFINED;
    }
    return CH_VOID;
}

static ChValue prim_write_string(ChVM *vm, ChValue *args, int nargs) {
    /* (write-string string [port [start [end]]]) — start/end are byte offsets. */
    if (nargs < 1 || !ch_is_string(args[0])) {
        snprintf(vm->error, sizeof(vm->error), "write-string: expected string");
        return CH_UNDEFINED;
    }
    ChPort *p = require_output_port(vm, args, nargs, 1);
    if (!p) {
        return CH_UNDEFINED;
    }
    ChString *s = ch_as_string(args[0]);
    size_t start = 0;
    size_t end = s->len;
    int range_arg = (nargs > 1 && ch_is_port(args[1])) ? 2 : 1;
    if (nargs > range_arg) {
        if (!ch_is_fixnum(args[range_arg])) {
            snprintf(vm->error, sizeof(vm->error), "write-string: bad start");
            return CH_UNDEFINED;
        }
        int64_t st = ch_to_fixnum(args[range_arg]);
        if (st < 0 || (size_t)st > s->len) {
            snprintf(vm->error, sizeof(vm->error), "write-string: start out of range");
            return CH_UNDEFINED;
        }
        start = (size_t)st;
    }
    if (nargs > range_arg + 1) {
        if (!ch_is_fixnum(args[range_arg + 1])) {
            snprintf(vm->error, sizeof(vm->error), "write-string: bad end");
            return CH_UNDEFINED;
        }
        int64_t en = ch_to_fixnum(args[range_arg + 1]);
        if (en < (int64_t)start || (size_t)en > s->len) {
            snprintf(vm->error, sizeof(vm->error), "write-string: end out of range");
            return CH_UNDEFINED;
        }
        end = (size_t)en;
    }
    if (end > start && port_write_bytes(p, s->data + start, end - start) != 0) {
        snprintf(vm->error, sizeof(vm->error), "write-string: write failed");
        return CH_UNDEFINED;
    }
    return CH_VOID;
}

static ChValue prim_read_line(ChVM *vm, ChValue *args, int nargs) {
    ChPort *p = require_input_port(vm, args, nargs, 0);
    if (!p) {
        return CH_UNDEFINED;
    }
    char *buf = NULL;
    size_t len = 0;
    size_t cap = 0;
    for (;;) {
        int c = port_read_byte(vm, p);
        if (c == -2) {
            free(buf);
            return CH_UNDEFINED;
        }
        if (c < 0) {
            if (len == 0) {
                free(buf);
                return CH_EOF_OBJ;
            }
            break;
        }
        if (c == '\n') {
            break;
        }
        if (c == '\r') {
            int peek = port_peek_byte(vm, p);
            if (peek == -2) {
                free(buf);
                return CH_UNDEFINED;
            }
            if (peek == '\n') {
                (void)port_read_byte(vm, p);
            }
            break;
        }
        if (len + 1 >= cap) {
            size_t ncap = cap ? cap * 2 : 64;
            char *nb = (char *)realloc(buf, ncap);
            if (!nb) {
                free(buf);
                abort();
            }
            buf = nb;
            cap = ncap;
        }
        buf[len++] = (char)c;
    }
    if (buf) {
        buf[len] = '\0';
    }
    ChValue out = ch_gc_make_string(&vm->gc, buf ? buf : "", len);
    free(buf);
    return out;
}

static ChValue prim_read_string(ChVM *vm, ChValue *args, int nargs) {
    if (nargs < 1 || !ch_is_fixnum(args[0])) {
        snprintf(vm->error, sizeof(vm->error), "read-string: expected k");
        return CH_UNDEFINED;
    }
    int64_t k = ch_to_fixnum(args[0]);
    if (k < 0) {
        snprintf(vm->error, sizeof(vm->error), "read-string: negative length");
        return CH_UNDEFINED;
    }
    ChPort *p = require_input_port(vm, args, nargs, 1);
    if (!p) {
        return CH_UNDEFINED;
    }
    if (k == 0) {
        return ch_gc_make_string(&vm->gc, "", 0);
    }
    char *buf = NULL;
    size_t len = 0;
    size_t cap = 0;
    for (int64_t i = 0; i < k; i++) {
        uint32_t cp = 0;
        size_t next = 0;
        int rc = port_decode_utf8_at(vm, p, p->pos, &cp, &next);
        if (rc == -2) {
            free(buf);
            return CH_UNDEFINED;
        }
        if (rc != 0) {
            if (i == 0) {
                free(buf);
                return CH_EOF_OBJ;
            }
            break;
        }
        p->pos = next;
        char encoded[4];
        size_t n = next; /* placeholder */
        /* re-encode from cp */
        if (cp <= 0x7Fu) {
            encoded[0] = (char)cp;
            n = 1;
        } else if (cp <= 0x7FFu) {
            encoded[0] = (char)(0xC0u | (cp >> 6));
            encoded[1] = (char)(0x80u | (cp & 0x3Fu));
            n = 2;
        } else if (cp <= 0xFFFFu) {
            encoded[0] = (char)(0xE0u | (cp >> 12));
            encoded[1] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
            encoded[2] = (char)(0x80u | (cp & 0x3Fu));
            n = 3;
        } else {
            encoded[0] = (char)(0xF0u | (cp >> 18));
            encoded[1] = (char)(0x80u | ((cp >> 12) & 0x3Fu));
            encoded[2] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
            encoded[3] = (char)(0x80u | (cp & 0x3Fu));
            n = 4;
        }
        if (len + n >= cap) {
            size_t ncap = cap ? cap * 2 : 64;
            while (len + n >= ncap) {
                ncap *= 2;
            }
            char *nb = (char *)realloc(buf, ncap);
            if (!nb) {
                free(buf);
                abort();
            }
            buf = nb;
            cap = ncap;
        }
        memcpy(buf + len, encoded, n);
        len += n;
    }
    if (buf) {
        buf[len] = '\0';
    }
    ChValue out = ch_gc_make_string(&vm->gc, buf ? buf : "", len);
    free(buf);
    return out;
}

static int parse_port_slice(ChVM *vm, ChValue *args, int nargs, int start_arg, size_t len, const char *who,
                            size_t *start_out, size_t *end_out) {
    size_t start = 0;
    size_t end = len;
    if (nargs > start_arg) {
        if (parse_nonnegative_fixnum(vm, args[start_arg], &start, who) != 0) {
            return -1;
        }
    }
    if (nargs > start_arg + 1) {
        if (parse_nonnegative_fixnum(vm, args[start_arg + 1], &end, who) != 0) {
            return -1;
        }
    }
    if (start > end || end > len) {
        snprintf(vm->error, sizeof(vm->error), "%s: slice out of range", who);
        return -1;
    }
    *start_out = start;
    *end_out = end;
    return 0;
}

static ChValue prim_read_u8(ChVM *vm, ChValue *args, int nargs) {
    ChPort *p = require_input_port(vm, args, nargs, 0);
    if (!p) {
        return CH_UNDEFINED;
    }
    int c = port_read_byte(vm, p);
    if (c == -2) {
        return CH_UNDEFINED;
    }
    if (c < 0) {
        return CH_EOF_OBJ;
    }
    return ch_make_fixnum(c);
}

static ChValue prim_peek_u8(ChVM *vm, ChValue *args, int nargs) {
    ChPort *p = require_input_port(vm, args, nargs, 0);
    if (!p) {
        return CH_UNDEFINED;
    }
    int c = port_peek_byte(vm, p);
    if (c == -2) {
        return CH_UNDEFINED;
    }
    if (c < 0) {
        return CH_EOF_OBJ;
    }
    return ch_make_fixnum(c);
}

static ChValue prim_write_u8(ChVM *vm, ChValue *args, int nargs) {
    uint8_t b = 0;
    if (parse_u8(vm, args[0], &b, "write-u8") != 0) {
        return CH_UNDEFINED;
    }
    ChPort *p = require_output_port(vm, args, nargs, 1);
    if (!p) {
        return CH_UNDEFINED;
    }
    char byte = (char)b;
    if (port_write_bytes(p, &byte, 1) != 0) {
        snprintf(vm->error, sizeof(vm->error), "write-u8: write failed");
        return CH_UNDEFINED;
    }
    return CH_VOID;
}

static ChValue prim_u8_ready_p(ChVM *vm, ChValue *args, int nargs) {
    ChPort *p = require_input_port(vm, args, nargs, 0);
    if (!p) {
        return CH_UNDEFINED;
    }
    (void)p;
    return CH_TRUE;
}

static ChValue prim_read_bytevector(ChVM *vm, ChValue *args, int nargs) {
    if (nargs > 2) {
        snprintf(vm->error, sizeof(vm->error), "read-bytevector: expected 1 or 2 arguments");
        return CH_UNDEFINED;
    }
    size_t k = 0;
    if (parse_nonnegative_fixnum(vm, args[0], &k, "read-bytevector") != 0) {
        return CH_UNDEFINED;
    }
    ChPort *p = require_input_port(vm, args, nargs, 1);
    if (!p) {
        return CH_UNDEFINED;
    }
    if (k == 0) {
        return ch_gc_make_bytevector(&vm->gc, 0, 0);
    }
    size_t cap = k < 256 ? k : 256;
    if (cap == 0) {
        cap = 1;
    }
    uint8_t *buf = (uint8_t *)malloc(cap);
    if (!buf) {
        abort();
    }
    size_t n = 0;
    while (n < k) {
        int c = port_read_byte(vm, p);
        if (c == -2) {
            free(buf);
            return CH_UNDEFINED;
        }
        if (c < 0) {
            break;
        }
        if (n == cap) {
            size_t ncap = cap * 2;
            if (ncap > k) {
                ncap = k;
            }
            uint8_t *nb = (uint8_t *)realloc(buf, ncap);
            if (!nb) {
                free(buf);
                abort();
            }
            buf = nb;
            cap = ncap;
        }
        buf[n++] = (uint8_t)c;
    }
    if (n == 0) {
        free(buf);
        return CH_EOF_OBJ;
    }
    ChValue out = ch_gc_make_bytevector(&vm->gc, n, 0);
    ChBytevector *bv = ch_as_bytevector(out);
    memcpy(bv->data, buf, n);
    free(buf);
    return out;
}

static ChValue prim_read_bytevector_bang(ChVM *vm, ChValue *args, int nargs) {
    if (nargs > 4) {
        snprintf(vm->error, sizeof(vm->error), "read-bytevector!: expected 1 to 4 arguments");
        return CH_UNDEFINED;
    }
    if (!ch_is_bytevector(args[0])) {
        snprintf(vm->error, sizeof(vm->error), "read-bytevector!: not a bytevector");
        return CH_UNDEFINED;
    }
    ChBytevector *target = ch_as_bytevector(args[0]);
    if (ch_object_is_immutable(&target->header)) {
        snprintf(vm->error, sizeof(vm->error), "read-bytevector!: immutable bytevector");
        return CH_UNDEFINED;
    }
    ChPort *p = require_input_port(vm, args, nargs, 1);
    if (!p) {
        return CH_UNDEFINED;
    }
    size_t start = 0;
    size_t end = target->len;
    if (parse_port_slice(vm, args, nargs, 2, target->len, "read-bytevector!", &start, &end) != 0) {
        return CH_UNDEFINED;
    }
    size_t count = end - start;
    if (count == 0) {
        return ch_make_fixnum(0);
    }
    size_t n = 0;
    while (n < count) {
        int c = port_read_byte(vm, p);
        if (c == -2) {
            return CH_UNDEFINED;
        }
        if (c < 0) {
            break;
        }
        target->data[start + n] = (uint8_t)c;
        n++;
    }
    if (n == 0) {
        return CH_EOF_OBJ;
    }
    return ch_make_fixnum((int64_t)n);
}

static ChValue prim_write_bytevector(ChVM *vm, ChValue *args, int nargs) {
    if (nargs > 4) {
        snprintf(vm->error, sizeof(vm->error), "write-bytevector: expected 1 to 4 arguments");
        return CH_UNDEFINED;
    }
    if (!ch_is_bytevector(args[0])) {
        snprintf(vm->error, sizeof(vm->error), "write-bytevector: not a bytevector");
        return CH_UNDEFINED;
    }
    ChBytevector *bv = ch_as_bytevector(args[0]);
    ChPort *p = require_output_port(vm, args, nargs, 1);
    if (!p) {
        return CH_UNDEFINED;
    }
    size_t start = 0;
    size_t end = bv->len;
    if (parse_port_slice(vm, args, nargs, 2, bv->len, "write-bytevector", &start, &end) != 0) {
        return CH_UNDEFINED;
    }
    if (end > start && port_write_bytes(p, (const char *)bv->data + start, end - start) != 0) {
        snprintf(vm->error, sizeof(vm->error), "write-bytevector: write failed");
        return CH_UNDEFINED;
    }
    return CH_VOID;
}

static bool reader_refill_from_port(ChReader *r, void *ctx) {
    ChPort *p = (ChPort *)ctx;
    if (!p || p->closed || !p->input || (p->kind != CH_PORT_STDIO && p->kind != CH_PORT_FILE)) {
        return false;
    }
    int appended = append_file_input_bytes(NULL, p, 0);
    r->src = p->buf ? p->buf + p->pos : "";
    r->len = p->len - p->pos;
    return appended > 0;
}

static ChValue prim_read(ChVM *vm, ChValue *args, int nargs) {
    ChPort *p = require_input_port(vm, args, nargs, 0);
    if (!p) {
        return CH_UNDEFINED;
    }
    if (p->kind == CH_PORT_STDIO || p->kind == CH_PORT_FILE) {
        compact_port_input_buffer(p);
    }

    ChReader r;
    ch_reader_init(&r, &vm->gc, p->buf ? p->buf + p->pos : "", p->len - p->pos);
    if (p->kind == CH_PORT_STDIO || p->kind == CH_PORT_FILE) {
        ch_reader_set_refill(&r, reader_refill_from_port, p);
    }

    ChValue out = CH_NIL;
    ch_gc_push(&vm->gc, &out);
    ChReadStatus st = ch_read_datum(&r, &out);
    ch_gc_pop(&vm->gc);
    if (st == CH_READ_EOF) {
        p->pos += r.pos;
        return CH_EOF_OBJ;
    }
    if (st != CH_READ_OK) {
        return raise_read_error(vm, ch_reader_error(&r),
                                (nargs > 0) ? args[0] : get_global(vm, "current-input-port"));
    }
    p->pos += r.pos;
    if (p->kind == CH_PORT_STDIO || p->kind == CH_PORT_FILE) {
        compact_port_input_buffer(p);
    }
    return out;
}

static bool port_can_position(ChPort *port) {
    if (port->closed) {
        return false;
    }
    switch (port->kind) {
    case CH_PORT_STRING_IN:
    case CH_PORT_STRING_OUT:
    case CH_PORT_BYTEVECTOR:
        return true;
    case CH_PORT_FILE:
        if (port->file) {
            long cur = ftell(port->file);
            return cur >= 0;
        }
        return false;
    default:
        return false;
    }
}

static ChValue prim_port_position(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    if (!ch_is_port(args[0])) {
        snprintf(vm->error, sizeof(vm->error), "port-position: expected port");
        return CH_UNDEFINED;
    }
    ChPort *port = ch_as_port(args[0]);
    if (port->closed) {
        snprintf(vm->error, sizeof(vm->error), "port-position: port is closed");
        return CH_UNDEFINED;
    }
    if (!port_can_position(port)) {
        snprintf(vm->error, sizeof(vm->error), "port-position: port does not support positioning");
        return CH_UNDEFINED;
    }
    if (port->kind == CH_PORT_FILE && port->file) {
        long pos = ftell(port->file);
        if (pos < 0) {
            snprintf(vm->error, sizeof(vm->error), "port-position: port does not support positioning");
            return CH_UNDEFINED;
        }
        return ch_make_fixnum(pos);
    }
    return ch_make_fixnum((int64_t)port->pos);
}

static ChValue prim_set_port_position_bang(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    if (!ch_is_port(args[0])) {
        snprintf(vm->error, sizeof(vm->error), "set-port-position!: expected port");
        return CH_UNDEFINED;
    }
    if (!ch_is_fixnum(args[1])) {
        snprintf(vm->error, sizeof(vm->error), "set-port-position!: expected exact integer");
        return CH_UNDEFINED;
    }
    int64_t pos = ch_to_fixnum(args[1]);
    if (pos < 0) {
        snprintf(vm->error, sizeof(vm->error), "set-port-position!: expected non-negative integer");
        return CH_UNDEFINED;
    }
    ChPort *port = ch_as_port(args[0]);
    if (port->closed) {
        snprintf(vm->error, sizeof(vm->error), "set-port-position!: port is closed");
        return CH_UNDEFINED;
    }
    if (port->output && port->kind == CH_PORT_FILE && port->file) {
        fflush(port->file);
    }
    if (port->kind == CH_PORT_FILE && port->file) {
        if (fseek(port->file, pos, SEEK_SET) != 0) {
            snprintf(vm->error, sizeof(vm->error),
                     "set-port-position!: invalid position or port does not support positioning");
            return CH_UNDEFINED;
        }
        return CH_VOID;
    }
    if (port->kind == CH_PORT_STRING_IN || port->kind == CH_PORT_STRING_OUT ||
        port->kind == CH_PORT_BYTEVECTOR) {
        if ((size_t)pos > port->len) {
            snprintf(vm->error, sizeof(vm->error), "set-port-position!: index out of range");
            return CH_UNDEFINED;
        }
        port->pos = (size_t)pos;
        return CH_VOID;
    }
    snprintf(vm->error, sizeof(vm->error),
             "set-port-position!: invalid position or port does not support positioning");
    return CH_UNDEFINED;
}

static ChValue prim_port_has_port_position_p(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    if (!ch_is_port(args[0])) {
        snprintf(vm->error, sizeof(vm->error), "port-has-port-position?: expected port");
        return CH_UNDEFINED;
    }
    return port_can_position(ch_as_port(args[0])) ? CH_TRUE : CH_FALSE;
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
    define_prim(vm, "read-char", prim_read_char, -1, 0);
    define_prim(vm, "peek-char", prim_peek_char, -1, 0);
    define_prim(vm, "char-ready?", prim_char_ready_p, -1, 0);
    define_prim(vm, "write-char", prim_write_char, -1, 1);
    define_prim(vm, "write-string", prim_write_string, -1, 1);
    define_prim(vm, "read-line", prim_read_line, -1, 0);
    define_prim(vm, "read-string", prim_read_string, -1, 1);
    define_prim(vm, "read-u8", prim_read_u8, -1, 0);
    define_prim(vm, "peek-u8", prim_peek_u8, -1, 0);
    define_prim(vm, "write-u8", prim_write_u8, -1, 1);
    define_prim(vm, "u8-ready?", prim_u8_ready_p, -1, 0);
    define_prim(vm, "read-bytevector", prim_read_bytevector, -1, 1);
    define_prim(vm, "read-bytevector!", prim_read_bytevector_bang, -1, 1);
    define_prim(vm, "write-bytevector", prim_write_bytevector, -1, 1);
    define_prim(vm, "read", prim_read, -1, 0);

    /* Replace core display/write/newline with port-aware versions. */
    define_prim(vm, "display", prim_display, -1, 1);
    define_prim(vm, "write", prim_write, -1, 1);
    define_prim(vm, "write-shared", prim_write_shared, -1, 1);
    define_prim(vm, "write-simple", prim_write_simple, -1, 1);
    define_prim(vm, "newline", prim_newline, -1, 0);
    define_prim(vm, "port-position", prim_port_position, 1, 1);
    define_prim(vm, "set-port-position!", prim_set_port_position_bang, 2, 2);
    define_prim(vm, "port-has-port-position?", prim_port_has_port_position_p, 1, 1);
    define_prim(vm, "port-has-set-port-position!?", prim_port_has_port_position_p, 1, 1);

    /* Current ports are parameter objects (R7RS dynamic binding). */
    set_global(vm, "current-input-port", in_param);
    set_global(vm, "current-output-port", out_param);
    set_global(vm, "current-error-port", err_param);
    set_global(vm, "%current-input-port", in_param);
    set_global(vm, "%current-output-port", out_param);
    set_global(vm, "%current-error-port", err_param);
    ch_gc_pop_n(&vm->gc, 6);
}
