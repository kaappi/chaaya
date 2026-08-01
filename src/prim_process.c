#include "chaaya/prim.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>

extern char **environ;
#endif

static void define_prim(ChVM *vm, const char *name, ChNativeFn fn, int arity, int min_arity) {
    ChValue sym = ch_gc_intern_symbol_cstr(&vm->gc, name);
    int idx = ch_vm_intern_global(vm, ch_as_symbol(sym));
    ChValue nv = ch_gc_make_native(&vm->gc, fn, name, arity, min_arity);
    ch_vm_define_global(vm, idx, nv);
}

static int exit_status_from_args(ChValue *args, int nargs) {
    if (nargs >= 1) {
        if (ch_is_fixnum(args[0])) {
            return (int)(ch_to_fixnum(args[0]) & 0xFF);
        }
        if (args[0] == CH_FALSE) {
            return 1;
        }
        if (args[0] != CH_TRUE) {
            return 1;
        }
    }
    return 0;
}

static ChValue prim_emergency_exit(ChVM *vm, ChValue *args, int nargs) {
    (void)vm;
    _Exit(exit_status_from_args(args, nargs));
    return CH_VOID; /* unreachable */
}

static ChValue prim_get_environment_variable(ChVM *vm, ChValue *args, int nargs) {
    (void)nargs;
    if (!ch_is_string(args[0])) {
        snprintf(vm->error, sizeof(vm->error), "get-environment-variable: expected string");
        return CH_UNDEFINED;
    }
    ChString *name = ch_as_string(args[0]);
    char *name_z = (char *)malloc(name->len + 1);
    if (!name_z) {
        snprintf(vm->error, sizeof(vm->error), "get-environment-variable: out of memory");
        return CH_UNDEFINED;
    }
    memcpy(name_z, name->data, name->len);
    name_z[name->len] = '\0';

#if defined(_WIN32)
    DWORD needed = GetEnvironmentVariableA(name_z, NULL, 0);
    if (needed == 0) {
        free(name_z);
        return CH_FALSE;
    }
    char *buf = (char *)malloc(needed);
    if (!buf) {
        free(name_z);
        snprintf(vm->error, sizeof(vm->error), "get-environment-variable: out of memory");
        return CH_UNDEFINED;
    }
    if (GetEnvironmentVariableA(name_z, buf, needed) == 0) {
        free(buf);
        free(name_z);
        return CH_FALSE;
    }
    ChValue out = ch_gc_make_string_cstr(&vm->gc, buf);
    free(buf);
    free(name_z);
    return out;
#else
    const char *val = getenv(name_z);
    free(name_z);
    if (!val) {
        return CH_FALSE;
    }
    return ch_gc_make_string_cstr(&vm->gc, val);
#endif
}

static ChValue prim_get_environment_variables(ChVM *vm, ChValue *args, int nargs) {
    (void)args;
    (void)nargs;

    ChValue result = CH_NIL;
    ch_gc_push(&vm->gc, &result);

#if defined(_WIN32)
    LPCH block = GetEnvironmentStringsA();
    if (!block) {
        ch_gc_pop(&vm->gc);
        snprintf(vm->error, sizeof(vm->error), "get-environment-variables: could not read environment");
        return CH_UNDEFINED;
    }
    for (LPCH p = block; *p; p += strlen(p) + 1) {
        const char *eq = strchr(p, '=');
        if (!eq || eq == p) {
            continue;
        }
        ChValue key = ch_gc_make_string(&vm->gc, p, (size_t)(eq - p));
        ch_gc_push(&vm->gc, &key);
        ChValue val = ch_gc_make_string_cstr(&vm->gc, eq + 1);
        ch_gc_push(&vm->gc, &val);
        ChValue pair = ch_gc_cons(&vm->gc, val, CH_NIL);
        ch_gc_push(&vm->gc, &pair);
        pair = ch_gc_cons(&vm->gc, key, pair);
        result = ch_gc_cons(&vm->gc, pair, result);
        ch_gc_pop_n(&vm->gc, 3);
    }
    FreeEnvironmentStringsA(block);
#else
    if (environ) {
        for (char **p = environ; *p; p++) {
            const char *eq = strchr(*p, '=');
            if (!eq) {
                continue;
            }
            ChValue key = ch_gc_make_string(&vm->gc, *p, (size_t)(eq - *p));
            ch_gc_push(&vm->gc, &key);
            ChValue val = ch_gc_make_string_cstr(&vm->gc, eq + 1);
            ch_gc_push(&vm->gc, &val);
            ChValue pair = ch_gc_cons(&vm->gc, val, CH_NIL);
            ch_gc_push(&vm->gc, &pair);
            pair = ch_gc_cons(&vm->gc, key, pair);
            result = ch_gc_cons(&vm->gc, pair, result);
            ch_gc_pop_n(&vm->gc, 3);
        }
    }
#endif

    ch_gc_pop(&vm->gc);
    return result;
}

void ch_register_process_primitives(ChVM *vm) {
    define_prim(vm, "get-environment-variable", prim_get_environment_variable, 1, 1);
    define_prim(vm, "get-environment-variables", prim_get_environment_variables, 0, 0);
    define_prim(vm, "emergency-exit", prim_emergency_exit, -1, 0);
}
