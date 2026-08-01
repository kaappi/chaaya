#include "chaaya/compiler.h"
#include "chaaya/printer.h"
#include "chaaya/reader.h"
#include "chaaya/vm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHAAYA_VERSION "0.1.0"

static int eval_source(ChVM *vm, const char *source, size_t len, bool print_results) {
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
        if (print_results && result != CH_VOID) {
            ch_print_value(stdout, result, false);
            fputc('\n', stdout);
        }
        ch_gc_pop_n(&vm->gc, 2); /* result, expr */
    }
    return 0;
}

static char *read_file(const char *path, size_t *out_len) {
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

static int run_repl(ChVM *vm) {
    int interactive = isatty(STDIN_FILENO);
    if (interactive) {
        printf("Chaaya %s — R7RS Scheme in C (bootstrap)\n", CHAAYA_VERSION);
        printf("Type expressions; Ctrl-D to exit.\n");
    }
    char line[4096];
    for (;;) {
        if (interactive) {
            fputs("> ", stdout);
            fflush(stdout);
        }
        if (!fgets(line, sizeof(line), stdin)) {
            if (interactive) {
                fputc('\n', stdout);
            }
            break;
        }
        if (line[0] == '\0' || line[0] == '\n') {
            continue;
        }
        eval_source(vm, line, strlen(line), true);
        vm->error[0] = '\0';
    }
    return 0;
}

static void usage(const char *argv0) {
    fprintf(stderr, "Usage: %s [options] [file.scm]\n", argv0);
    fprintf(stderr, "  -h, --help     Show help\n");
    fprintf(stderr, "  -v, --version  Show version\n");
    fprintf(stderr, "  (no file)      Start REPL\n");
}

int main(int argc, char **argv) {
    const char *file = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        }
        if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--version") == 0) {
            printf("chaaya %s\n", CHAAYA_VERSION);
            return 0;
        }
        if (argv[i][0] == '-') {
            fprintf(stderr, "unknown option: %s\n", argv[i]);
            usage(argv[0]);
            return 1;
        }
        if (file) {
            fprintf(stderr, "extra argument: %s\n", argv[i]);
            return 1;
        }
        file = argv[i];
    }

    ChVM vm;
    ch_vm_init(&vm);
    ch_vm_register_primitives(&vm);

    int rc = 0;
    if (!file) {
        rc = run_repl(&vm);
    } else {
        size_t len = 0;
        char *src = read_file(file, &len);
        if (!src) {
            fprintf(stderr, "cannot read file: %s\n", file);
            ch_vm_deinit(&vm);
            return 1;
        }
        rc = eval_source(&vm, src, len, false);
        free(src);
    }

    ch_vm_deinit(&vm);
    return rc;
}
