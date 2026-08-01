#include "chaaya/eval.h"

#include "chaaya/compiler.h"
#include "chaaya/printer.h"
#include "chaaya/reader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void ch_vm_set_global_cstr(ChVM *vm, const char *name, ChValue v) {
    ChValue sym = ch_gc_intern_symbol_cstr(&vm->gc, name);
    int idx = ch_vm_intern_global(vm, ch_as_symbol(sym));
    ch_vm_define_global(vm, idx, v);
}

const char *ch_value_type_name(ChValue v) {
    if (ch_is_nil(v)) {
        return "null";
    }
    if (v == CH_TRUE || v == CH_FALSE) {
        return "boolean";
    }
    if (v == CH_VOID) {
        return "void";
    }
    if (v == CH_EOF_OBJ) {
        return "eof";
    }
    if (ch_is_char(v)) {
        return "char";
    }
    if (ch_is_fixnum(v)) {
        return "fixnum";
    }
    if (ch_is_flonum(v)) {
        return "flonum";
    }
    if (ch_is_pair(v)) {
        return "pair";
    }
    if (ch_is_symbol(v)) {
        return "symbol";
    }
    if (ch_is_string(v)) {
        return "string";
    }
    if (ch_is_vector(v)) {
        return "vector";
    }
    if (ch_is_closure(v)) {
        return "closure";
    }
    if (ch_is_native(v)) {
        return "native";
    }
    if (ch_is_function(v)) {
        return "function";
    }
    return "unknown";
}

int ch_eval_source(ChVM *vm, const char *source, size_t len, int print_results) {
    ChReader reader;
    ch_reader_init(&reader, &vm->gc, source, len);
    ChCompiler compiler;
    ch_compiler_init(&compiler, vm);

    for (;;) {
        ChValue expr = CH_NIL;
        ch_gc_push(&vm->gc, &expr);
        for (size_t i = 0; i < vm->global_count; i++) {
            ch_gc_push(&vm->gc, &vm->globals[i].value);
        }
        ChReadStatus rs = ch_read_datum(&reader, &expr);
        ch_gc_pop_n(&vm->gc, vm->global_count);
        if (rs == CH_READ_EOF) {
            ch_gc_pop(&vm->gc);
            break;
        }
        if (rs == CH_READ_ERROR) {
            fprintf(stderr, "read error: %s\n", ch_reader_error(&reader));
            ch_gc_pop(&vm->gc);
            return 1;
        }

        ChFunction *fn = NULL;
        if (ch_compile_toplevel(&compiler, expr, &fn) != CH_COMPILE_OK) {
            fprintf(stderr, "compile error: %s\n", ch_compiler_error(&compiler));
            ch_gc_pop(&vm->gc);
            return 1;
        }

        ChValue result = CH_VOID;
        ch_gc_push(&vm->gc, &result);
        ChVMStatus vs = ch_vm_eval_function(vm, fn, &result);
        if (vs != CH_VM_OK) {
            fprintf(stderr, "runtime error: %s\n", ch_vm_error(vm));
            ch_gc_pop_n(&vm->gc, 2);
            return 1;
        }
        if (vm->error[0] != '\0' && result == CH_UNDEFINED) {
            fprintf(stderr, "runtime error: %s\n", ch_vm_error(vm));
            ch_gc_pop_n(&vm->gc, 2);
            return 1;
        }
        if (result != CH_VOID) {
            ch_vm_set_global_cstr(vm, "_", result);
            if (print_results) {
                ch_print_value(stdout, result, false);
                fputc('\n', stdout);
            }
        }
        ch_gc_pop_n(&vm->gc, 2);
    }
    return 0;
}

char *ch_read_file(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long sz = ftell(f);
    if (sz < 0) {
        fclose(f);
        return NULL;
    }
    rewind(f);
    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[n] = '\0';
    *out_len = n;
    return buf;
}
