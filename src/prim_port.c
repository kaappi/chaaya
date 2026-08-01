#include "chaaya/prim.h"

#include "chaaya/printer.h"
#include "chaaya/reader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

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

static int port_write_bytes(ChPort *p, const char *data, size_t len) {
    if (p->closed || !p->output) {
        return -1;
    }
    if (p->kind == CH_PORT_STDIO || p->kind == CH_PORT_FILE) {
        return fwrite(data, 1, len, p->file) == len ? 0 : -1;
    }
    if (p->kind == CH_PORT_STRING_OUT) {
        while (p->len + len + 1 > p->cap) {
            size_t ncap = p->cap * 2;
            char *nb = (char *)realloc(p->buf, ncap);
            if (!nb) {
                abort();
            }
            p->buf = nb;
            p->cap = ncap;
        }
        memcpy(p->buf + p->len, data, len);
        p->len += len;
        p->buf[p->len] = '\0';
        return 0;
    }
    return -1;
}

static ChPort *require_output_port(ChVM *vm, ChValue *args, int nargs, int idx) {
    ChValue pv;
    if (nargs > idx) {
        pv = args[idx];
    } else {
        pv = get_global(vm, "current-output-port");
        /* current-output-port may be the procedure — call it? R7RS uses parameter.
         * We bind the port object directly as a global value for bootstrap. */
        if (ch_is_native(pv) || ch_is_closure(pv)) {
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
        if (ch_is_native(pv) || ch_is_closure(pv)) {
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

static const char *require_path(ChVM *vm, ChValue v, const char *who) {
    if (!ch_is_string(v)) {
        snprintf(vm->error, sizeof(vm->error), "%s: expected string path", who);
        return NULL;
    }
    return ch_as_string(v)->data;
}

static ChValue prim_open_input_file(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    const char *path = require_path(vm, args[0], "open-input-file");
    if (!path) {
        return CH_UNDEFINED;
    }
    FILE *f = fopen(path, "rb");
    if (!f) {
        snprintf(vm->error, sizeof(vm->error), "open-input-file: cannot open %s", path);
        return CH_UNDEFINED;
    }
    return ch_gc_make_file_port(&vm->gc, f, 1, 0);
}

static ChValue prim_open_output_file(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    const char *path = require_path(vm, args[0], "open-output-file");
    if (!path) {
        return CH_UNDEFINED;
    }
    FILE *f = fopen(path, "wb");
    if (!f) {
        snprintf(vm->error, sizeof(vm->error), "open-output-file: cannot open %s", path);
        return CH_UNDEFINED;
    }
    return ch_gc_make_file_port(&vm->gc, f, 0, 1);
}

static ChValue prim_file_exists_p(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    const char *path = require_path(vm, args[0], "file-exists?");
    if (!path) {
        return CH_UNDEFINED;
    }
    struct stat st;
    return (stat(path, &st) == 0 && S_ISREG(st.st_mode)) ? CH_TRUE : CH_FALSE;
}

static ChValue prim_delete_file(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    const char *path = require_path(vm, args[0], "delete-file");
    if (!path) {
        return CH_UNDEFINED;
    }
    if (remove(path) != 0) {
        snprintf(vm->error, sizeof(vm->error), "delete-file: cannot delete %s", path);
        return CH_UNDEFINED;
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
    ChValue saved = get_global(vm, "%current-input-port");
    set_global(vm, "%current-input-port", port);
    ChValue result = CH_UNDEFINED;
    ch_gc_push(&vm->gc, &port);
    ch_gc_push(&vm->gc, &saved);
    ch_gc_push(&vm->gc, &result);
    if (ch_vm_apply(vm, args[1], NULL, 0, &result) != CH_VM_OK) {
        set_global(vm, "%current-input-port", saved);
        close_port_impl(ch_as_port(port));
        ch_gc_pop_n(&vm->gc, 3);
        return CH_UNDEFINED;
    }
    set_global(vm, "%current-input-port", saved);
    close_port_impl(ch_as_port(port));
    ch_gc_pop_n(&vm->gc, 3);
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
    ChValue saved = get_global(vm, "%current-output-port");
    set_global(vm, "%current-output-port", port);
    ChValue result = CH_UNDEFINED;
    ch_gc_push(&vm->gc, &port);
    ch_gc_push(&vm->gc, &saved);
    ch_gc_push(&vm->gc, &result);
    if (ch_vm_apply(vm, args[1], NULL, 0, &result) != CH_VM_OK) {
        set_global(vm, "%current-output-port", saved);
        close_port_impl(ch_as_port(port));
        ch_gc_pop_n(&vm->gc, 3);
        return CH_UNDEFINED;
    }
    set_global(vm, "%current-output-port", saved);
    close_port_impl(ch_as_port(port));
    ch_gc_pop_n(&vm->gc, 3);
    return result;
}

static void print_to_port(ChPort *p, ChValue v, bool display) {
    FILE *f = port_file(p);
    if (f) {
        ch_print_value(f, v, display);
        return;
    }
    char *s = ch_value_to_string(v, display);
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
    print_to_port(p, args[0], true);
    return CH_VOID;
}

static ChValue prim_write(ChVM *vm, ChValue *args, int nargs) {
    ChPort *p = require_output_port(vm, args, nargs, 1);
    if (!p) {
        return CH_UNDEFINED;
    }
    print_to_port(p, args[0], false);
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

static int port_peek_byte(ChPort *p) {
    if (p->closed || !p->input) {
        return -1;
    }
    if (p->kind == CH_PORT_STDIO || p->kind == CH_PORT_FILE) {
        int c = fgetc(p->file);
        if (c == EOF) {
            return -1;
        }
        ungetc(c, p->file);
        return c;
    }
    if (p->kind == CH_PORT_STRING_IN) {
        if (p->pos >= p->len) {
            return -1;
        }
        return (unsigned char)p->buf[p->pos];
    }
    return -1;
}

static int port_read_byte(ChPort *p) {
    if (p->closed || !p->input) {
        return -1;
    }
    if (p->kind == CH_PORT_STDIO || p->kind == CH_PORT_FILE) {
        int c = fgetc(p->file);
        return c == EOF ? -1 : c;
    }
    if (p->kind == CH_PORT_STRING_IN) {
        if (p->pos >= p->len) {
            return -1;
        }
        return (unsigned char)p->buf[p->pos++];
    }
    return -1;
}

static ChValue prim_read_char(ChVM *vm, ChValue *args, int nargs) {
    ChPort *p = require_input_port(vm, args, nargs, 0);
    if (!p) {
        return CH_UNDEFINED;
    }
    int c = port_read_byte(p);
    if (c < 0) {
        return CH_EOF_OBJ;
    }
    return ch_make_char((uint32_t)c);
}

static ChValue prim_peek_char(ChVM *vm, ChValue *args, int nargs) {
    ChPort *p = require_input_port(vm, args, nargs, 0);
    if (!p) {
        return CH_UNDEFINED;
    }
    int c = port_peek_byte(p);
    if (c < 0) {
        return CH_EOF_OBJ;
    }
    return ch_make_char((uint32_t)c);
}

static ChValue prim_read(ChVM *vm, ChValue *args, int nargs) {
    ChPort *p = require_input_port(vm, args, nargs, 0);
    if (!p) {
        return CH_UNDEFINED;
    }
    if (p->kind == CH_PORT_STRING_IN) {
        ChReader r;
        ch_reader_init(&r, &vm->gc, p->buf + p->pos, p->len - p->pos);
        ChValue out = CH_NIL;
        ch_gc_push(&vm->gc, &out);
        ChReadStatus st = ch_read_datum(&r, &out);
        ch_gc_pop(&vm->gc);
        if (st == CH_READ_EOF) {
            return CH_EOF_OBJ;
        }
        if (st != CH_READ_OK) {
            snprintf(vm->error, sizeof(vm->error), "read: %s", ch_reader_error(&r));
            return CH_UNDEFINED;
        }
        p->pos += r.pos;
        return out;
    }
    /* FILE*: buffer remaining input, parse one datum, seek back unread bytes. */
    long start = ftell(p->file);
    char *buf = NULL;
    size_t cap = 0, len = 0;
    int c;
    while ((c = fgetc(p->file)) != EOF) {
        if (len + 2 > cap) {
            cap = cap ? cap * 2 : 256;
            char *nb = (char *)realloc(buf, cap);
            if (!nb) {
                abort();
            }
            buf = nb;
        }
        buf[len++] = (char)c;
    }
    if (!buf || len == 0) {
        free(buf);
        return CH_EOF_OBJ;
    }
    buf[len] = '\0';
    ChReader r;
    ch_reader_init(&r, &vm->gc, buf, len);
    ChValue out = CH_NIL;
    ch_gc_push(&vm->gc, &out);
    ChReadStatus st = ch_read_datum(&r, &out);
    ch_gc_pop(&vm->gc);
    if (st == CH_READ_EOF) {
        free(buf);
        return CH_EOF_OBJ;
    }
    if (st != CH_READ_OK) {
        snprintf(vm->error, sizeof(vm->error), "read: %s", ch_reader_error(&r));
        free(buf);
        return CH_UNDEFINED;
    }
    if (start >= 0) {
        if (fseek(p->file, start + (long)r.pos, SEEK_SET) != 0) {
            /* Non-seekable: leave at EOF (bootstrap limitation). */
        }
    }
    free(buf);
    return out;
}

void ch_register_port_primitives(ChVM *vm) {
    ChValue in = ch_gc_make_stdio_port(&vm->gc, stdin, 1, 0);
    ChValue out = ch_gc_make_stdio_port(&vm->gc, stdout, 0, 1);
    ChValue err = ch_gc_make_stdio_port(&vm->gc, stderr, 0, 1);

    define_prim(vm, "port?", prim_port_p, 1, 1);
    define_prim(vm, "input-port?", prim_input_port_p, 1, 1);
    define_prim(vm, "output-port?", prim_output_port_p, 1, 1);
    define_prim(vm, "eof-object?", prim_eof_object_p, 1, 1);
    define_prim(vm, "eof-object", prim_eof_object, 0, 0);
    define_prim(vm, "current-input-port", prim_current_input_port, 0, 0);
    define_prim(vm, "current-output-port", prim_current_output_port, 0, 0);
    define_prim(vm, "current-error-port", prim_current_error_port, 0, 0);
    define_prim(vm, "open-input-string", prim_open_input_string, 1, 1);
    define_prim(vm, "open-output-string", prim_open_output_string, 0, 0);
    define_prim(vm, "get-output-string", prim_get_output_string, 1, 1);
    define_prim(vm, "close-port", prim_close_port, 1, 1);
    define_prim(vm, "close-input-port", prim_close_port, 1, 1);
    define_prim(vm, "close-output-port", prim_close_port, 1, 1);
    define_prim(vm, "open-input-file", prim_open_input_file, 1, 1);
    define_prim(vm, "open-output-file", prim_open_output_file, 1, 1);
    define_prim(vm, "file-exists?", prim_file_exists_p, 1, 1);
    define_prim(vm, "delete-file", prim_delete_file, 1, 1);
    define_prim(vm, "call-with-input-file", prim_call_with_input_file, 2, 2);
    define_prim(vm, "call-with-output-file", prim_call_with_output_file, 2, 2);
    define_prim(vm, "with-input-from-file", prim_with_input_from_file, 2, 2);
    define_prim(vm, "with-output-to-file", prim_with_output_to_file, 2, 2);
    define_prim(vm, "read-char", prim_read_char, -1, 0);
    define_prim(vm, "peek-char", prim_peek_char, -1, 0);
    define_prim(vm, "read", prim_read, -1, 0);

    /* Replace core display/write/newline with port-aware versions. */
    define_prim(vm, "display", prim_display, -1, 1);
    define_prim(vm, "write", prim_write, -1, 1);
    define_prim(vm, "newline", prim_newline, -1, 0);

    /* Hidden current port objects. */
    {
        ChValue sym = ch_gc_intern_symbol_cstr(&vm->gc, "%current-input-port");
        ch_vm_define_global(vm, ch_vm_intern_global(vm, ch_as_symbol(sym)), in);
        sym = ch_gc_intern_symbol_cstr(&vm->gc, "%current-output-port");
        ch_vm_define_global(vm, ch_vm_intern_global(vm, ch_as_symbol(sym)), out);
        sym = ch_gc_intern_symbol_cstr(&vm->gc, "%current-error-port");
        ch_vm_define_global(vm, ch_vm_intern_global(vm, ch_as_symbol(sym)), err);
    }
}
