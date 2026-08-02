#include "chaaya/eval.h"

#include "chaaya/compiler.h"
#include "chaaya/environment.h"
#include "chaaya/library.h"
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
    if (ch_is_continuation(v)) {
        return "continuation";
    }
    if (ch_is_function(v)) {
        return "function";
    }
    if (ch_is_environment(v)) {
        return "environment";
    }
    if (ch_is_error_object(v)) {
        return "error-object";
    }
    if (ch_is_parameter(v)) {
        return "parameter";
    }
    if (ch_is_hashtable(v)) {
        return "hashtable";
    }
    if (ch_is_bytevector(v)) {
        return "bytevector";
    }
    if (ch_is_time(v)) {
        return "time";
    }
    return "unknown";
}

static int is_toplevel_keyword(ChValue expr, const char *name) {
    return ch_is_pair(expr) && ch_is_symbol(ch_car(expr)) &&
           strcmp(ch_symbol_basename(ch_as_symbol(ch_car(expr))), name) == 0;
}

static int eval_import_into_env(ChVM *vm, ChValue sets, ChLibEnv *env) {
    while (ch_is_pair(sets)) {
        if (ch_import_set_into_env(vm, ch_car(sets), env) != 0) {
            return -1;
        }
        sets = ch_cdr(sets);
    }
    if (!ch_is_nil(sets)) {
        snprintf(vm->error, sizeof(vm->error), "import: improper list");
        return -1;
    }
    return 0;
}

int ch_eval_datum(ChVM *vm, ChValue expr, ChValue env_or_void, ChValue *out) {
    if (out) {
        *out = CH_VOID;
    }

    /* Capture before nested natives clear it. */
    bool was_tail = vm->native_was_tail;

    ChEnvironment *env_obj = NULL;
    ChLibEnv *eval_env = NULL;
    if (env_or_void != CH_VOID) {
        if (!ch_is_environment(env_or_void)) {
            snprintf(vm->error, sizeof(vm->error), "eval: expected environment");
            return -1;
        }
        env_obj = ch_as_environment(env_or_void);
        if (env_obj->kind != CH_ENV_INTERACTION) {
            eval_env = &env_obj->env;
        }
    }

    ChLibEnv *saved_active_env = vm->active_lib_env;
    ChEnvironment *saved_eval_env = vm->active_eval_env;
    vm->active_eval_env = NULL;
    if (env_obj && env_obj->kind != CH_ENV_INTERACTION) {
        vm->active_eval_env = env_obj;
    }
    vm->active_lib_env = eval_env;

    ChValue expr_root = expr;
    ch_gc_push(&vm->gc, &expr_root);

    int rc = 0;
    ChValue result = CH_VOID;

    if (is_toplevel_keyword(expr_root, "import")) {
        if (eval_env) {
            rc = eval_import_into_env(vm, ch_cdr(expr_root), eval_env);
        } else if (ch_handle_import(vm, ch_cdr(expr_root)) != 0) {
            rc = -1;
        }
    } else if (is_toplevel_keyword(expr_root, "define-library") ||
               is_toplevel_keyword(expr_root, "include") ||
               is_toplevel_keyword(expr_root, "include-ci") ||
               is_toplevel_keyword(expr_root, "cond-expand")) {
        if (ch_eval_toplevel_form(vm, expr_root) != 0) {
            rc = -1;
        }
    } else {
        ChCompiler compiler;
        ch_compiler_init(&compiler, vm);
        ChFunction *fn = NULL;
        if (ch_compile_toplevel(&compiler, expr_root, &fn) != CH_COMPILE_OK) {
            snprintf(vm->error, sizeof(vm->error), "%s", ch_compiler_error(&compiler));
            rc = -1;
        } else {
            ChValue fn_keep = ch_make_pointer(&fn->header);
            ch_gc_push(&vm->gc, &fn_keep);
            /* Tail eval in the interaction environment: trampoline the thunk
             * so recursive (eval ...) does not grow the C stack. */
            bool can_tail = was_tail && (!env_obj || env_obj->kind == CH_ENV_INTERACTION);
            if (can_tail) {
                ChValue cl = ch_gc_make_closure(&vm->gc, fn, NULL);
                vm->has_pending_call = true;
                vm->pending_call_tail = true;
                vm->pending_proc = cl;
                vm->pending_nargs = 0;
                result = CH_UNDEFINED;
                ch_gc_pop(&vm->gc);
            } else {
                ch_gc_push(&vm->gc, &result);
                ChVMStatus st = ch_vm_eval_function(vm, fn, &result);
                ch_gc_pop_n(&vm->gc, 2);
                if (st != CH_VM_OK || (vm->error[0] != '\0' && result == CH_UNDEFINED)) {
                    rc = -1;
                }
            }
        }
    }

    ch_gc_pop(&vm->gc);
    vm->active_lib_env = saved_active_env;
    vm->active_eval_env = saved_eval_env;

    if (rc != 0) {
        return -1;
    }
    if (out) {
        *out = result;
    }
    return 0;
}

int ch_eval_source(ChVM *vm, const char *source, size_t len, int print_results) {
    ChReader reader;
    ch_reader_init(&reader, &vm->gc, source, len);

    for (;;) {
        ChValue expr = CH_NIL;
        ch_gc_push(&vm->gc, &expr);
        /* Globals/macros/libraries are marked via ch_vm_mark_gc_roots /
         * ch_library_mark_gc_roots — do not push them all (hits CH_GC_ROOT_MAX
         * once several libraries each hold a full (scheme base) import). */
        ChReadStatus rs = ch_read_datum(&reader, &expr);
        if (rs == CH_READ_EOF) {
            ch_gc_pop(&vm->gc);
            break;
        }
        if (rs == CH_READ_ERROR) {
            fprintf(stderr, "read error: %s\n", ch_reader_error(&reader));
            ch_gc_pop(&vm->gc);
            return 1;
        }

        ChValue result = CH_VOID;
        if (ch_eval_datum(vm, expr, CH_VOID, &result) != 0) {
            fprintf(stderr, "error: %s\n", ch_vm_error(vm));
            ch_gc_pop(&vm->gc);
            return 1;
        }
        if (result != CH_VOID) {
            ch_vm_set_global_cstr(vm, "_", result);
            if (print_results) {
                ch_print_value(stdout, result, false);
                fputc('\n', stdout);
            }
        }
        ch_gc_pop(&vm->gc);
    }
    return 0;
}

int ch_eval_file(ChVM *vm, const char *path, ChValue env_or_void, ChValue *last_out) {
    if (last_out) {
        *last_out = CH_VOID;
    }

    size_t len = 0;
    char *source = ch_read_file(path, &len);
    if (!source) {
        snprintf(vm->error, sizeof(vm->error), "load: cannot read %s", path);
        return -1;
    }

    ChReader reader;
    ch_reader_init(&reader, &vm->gc, source, len);

    ChValue last = CH_VOID;
    ch_gc_push(&vm->gc, &last);

    int rc = 0;
    for (;;) {
        ChValue expr = CH_NIL;
        ch_gc_push(&vm->gc, &expr);
        /* See ch_eval_source: VM/library mark roots cover globals and libs. */
        ChReadStatus rs = ch_read_datum(&reader, &expr);
        if (rs == CH_READ_EOF) {
            ch_gc_pop(&vm->gc);
            break;
        }
        if (rs != CH_READ_OK) {
            snprintf(vm->error, sizeof(vm->error), "load: read error in %s: %s", path,
                     ch_reader_error(&reader));
            ch_gc_pop(&vm->gc);
            rc = -1;
            break;
        }

        ChValue result = CH_VOID;
        if (ch_eval_datum(vm, expr, env_or_void, &result) != 0) {
            ch_gc_pop(&vm->gc);
            rc = -1;
            break;
        }
        last = result;
        ch_gc_pop(&vm->gc);
    }

    if (rc == 0 && last_out) {
        *last_out = last;
    }
    ch_gc_pop(&vm->gc);
    free(source);
    return rc;
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
