#include "chaaya/llvm_backend.h"

#include "chaaya/cli.h"
#include "chaaya/compiler.h"
#include "chaaya/eval.h"
#include "chaaya/expander.h"
#include "chaaya/ir.h"
#include "chaaya/reader.h"
#include "chaaya/version.h"
#include "chaaya/vm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void emit_ir_node(FILE *out, const ChIrNode *node, int *tmp) {
    if (!node) {
        return;
    }
    switch (node->kind) {
    case CH_IR_VOID:
        fprintf(out, "  ; void\n");
        break;
    case CH_IR_LITERAL:
        fprintf(out, "  ; literal\n");
        break;
    case CH_IR_VAR:
        fprintf(out, "  ; var %%%s\n", node->as.var ? node->as.var->name : "?");
        break;
    case CH_IR_IF: {
        int t = (*tmp)++;
        fprintf(out, "  ; if.%d\n", t);
        emit_ir_node(out, node->as.if_expr.test, tmp);
        emit_ir_node(out, node->as.if_expr.consequent, tmp);
        if (node->as.if_expr.has_alternate) {
            emit_ir_node(out, node->as.if_expr.alternate, tmp);
        }
        break;
    }
    case CH_IR_LAMBDA:
        fprintf(out, "  ; lambda\n");
        for (size_t i = 0; i < node->as.lambda.body_count; i++) {
            emit_ir_node(out, node->as.lambda.body[i], tmp);
        }
        break;
    case CH_IR_CALL:
        fprintf(out, "  ; call\n");
        emit_ir_node(out, node->as.call.callee, tmp);
        for (size_t i = 0; i < node->as.call.arg_count; i++) {
            emit_ir_node(out, node->as.call.args[i], tmp);
        }
        break;
    case CH_IR_PRIM_CALL:
        fprintf(out, "  ; prim %s\n",
                node->as.prim_call.symbol ? node->as.prim_call.symbol->name : "?");
        for (size_t i = 0; i < node->as.prim_call.arg_count; i++) {
            emit_ir_node(out, node->as.prim_call.args[i], tmp);
        }
        break;
    case CH_IR_DEFINE:
        fprintf(out, "  ; define\n");
        emit_ir_node(out, node->as.define_expr.value, tmp);
        break;
    case CH_IR_SEQ:
        for (size_t i = 0; i < node->as.seq.count; i++) {
            emit_ir_node(out, node->as.seq.items[i], tmp);
        }
        break;
    case CH_IR_RAW:
        fprintf(out, "  ; raw (legacy form)\n");
        break;
    default:
        fprintf(out, "  ; kind %d\n", (int)node->kind);
        break;
    }
}

static int lower_file_to_llvm(const char *path, FILE *out) {
    size_t len = 0;
    char *src = ch_read_file(path, &len);
    if (!src) {
        fprintf(stderr, "Error opening file '%s'\n", path);
        return CH_EXIT_ERROR;
    }

    fprintf(out, "; ModuleID = '%s'\n", path);
    fprintf(out, "; Chaaya %s — MVP LLVM IR emission (runtime calls stubbed)\n", CHAAYA_VERSION);
    fprintf(out, "target datalayout = \"e-m:o-i64:64-i128:128-n32:64-S128\"\n");
    fprintf(out, "target triple = \"unknown-unknown-unknown\"\n\n");
    fprintf(out, "declare i64 @ch_rt_main()\n\n");
    fprintf(out, "define i32 @main() {\nentry:\n");
    fprintf(out, "  %%r = call i64 @ch_rt_main()\n");

    ChVM vm;
    ch_vm_init(&vm);
    ch_vm_register_primitives(&vm);
    vm.script_path = path;

    ChReader reader;
    ch_reader_init(&reader, &vm.gc, src, len);
    int tmp = 0;
    int rc = CH_EXIT_OK;
    for (;;) {
        ChValue v = CH_NIL;
        ch_gc_push(&vm.gc, &v);
        ChReadStatus st = ch_read_datum(&reader, &v);
        if (st == CH_READ_EOF) {
            ch_gc_pop(&vm.gc);
            break;
        }
        if (st != CH_READ_OK) {
            fprintf(stderr, "emit-llvm: read error: %s\n", ch_reader_error(&reader));
            ch_gc_pop(&vm.gc);
            rc = CH_EXIT_ERROR;
            break;
        }
        ChValue expanded = CH_NIL;
        ch_gc_push(&vm.gc, &expanded);
        char err[256];
        if (ch_expand_toplevel(&vm, v, &expanded, err, sizeof(err)) != CH_EXPAND_OK) {
            fprintf(stderr, "emit-llvm: expand error: %s\n", err);
            ch_gc_pop_n(&vm.gc, 2);
            rc = CH_EXIT_ERROR;
            break;
        }
        ChCompiler compiler;
        ch_compiler_init(&compiler, &vm);
        ChIrNode *ir = NULL;
        if (ch_ir_lower(&compiler, expanded, &ir) != CH_COMPILE_OK) {
            fprintf(stderr, "emit-llvm: lower error: %s\n", ch_compiler_error(&compiler));
            ch_gc_pop_n(&vm.gc, 2);
            rc = CH_EXIT_ERROR;
            break;
        }
        ch_ir_analyze(ir);
        (void)ch_ir_optimize(&compiler, &ir);
        emit_ir_node(out, ir, &tmp);
        ch_ir_free(ir);
        ch_gc_pop_n(&vm.gc, 2);
    }

    fprintf(out, "  ret i32 0\n}\n");
    free(src);
    ch_vm_deinit(&vm);
    return rc;
}

int ch_llvm_backend_emit_ir(const char *path, const char *out_path) {
    FILE *out = stdout;
    int close_out = 0;
    if (out_path) {
        out = fopen(out_path, "w");
        if (!out) {
            fprintf(stderr, "emit-llvm: cannot write '%s'\n", out_path);
            return CH_EXIT_ERROR;
        }
        close_out = 1;
    }
    int rc = lower_file_to_llvm(path, out);
    if (close_out) {
        fclose(out);
        if (rc == CH_EXIT_OK) {
            fprintf(stderr, "emit-llvm: wrote %s\n", out_path);
        }
    }
    return rc;
}

int ch_llvm_backend_compile_native(const char *path, const char *out_path) {
    const char *bin = out_path ? out_path : "a.out";
    char ll_path[64];
    char rt_path[64];
    snprintf(ll_path, sizeof(ll_path), "/tmp/chaaya-%d.ll", (int)getpid());
    snprintf(rt_path, sizeof(rt_path), "/tmp/chaaya-rt-%d.c", (int)getpid());

    int rc = ch_llvm_backend_emit_ir(path, ll_path);
    if (rc != CH_EXIT_OK) {
        unlink(ll_path);
        return rc;
    }

    FILE *rtf = fopen(rt_path, "w");
    if (!rtf) {
        unlink(ll_path);
        fprintf(stderr, "compile: cannot create temp runtime file\n");
        return CH_EXIT_ERROR;
    }
    fputs("long ch_rt_main(void) { return 0; }\n"
          "int main(void) { return (int)ch_rt_main(); }\n",
          rtf);
    fclose(rtf);

    /* MVP: link the tiny C stub (full .ll linking needs an LLVM-capable driver). */
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "cc -O0 -o '%s' '%s'", bin, rt_path);
    int st = system(cmd);
    unlink(rt_path);
    if (st != 0) {
        fprintf(stderr,
                "compile: emitted LLVM IR to %s but failed to link a host stub with `cc`.\n",
                ll_path);
        return CH_EXIT_ERROR;
    }
    fprintf(stderr, "compile: wrote %s (stub runtime; IR at %s)\n", bin, ll_path);
    return CH_EXIT_OK;
}

int ch_llvm_backend_run_file(const char *path) {
    if (!path) {
        path = "<stdin>";
    }
    char bin[64];
    snprintf(bin, sizeof(bin), "/tmp/chaaya-run-%d", (int)getpid());
    int rc = ch_llvm_backend_compile_native(path, bin);
    if (rc != CH_EXIT_OK) {
        return rc;
    }
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "'%s'", bin);
    int st = system(cmd);
    unlink(bin);
    return st == 0 ? CH_EXIT_OK : CH_EXIT_ERROR;
}
