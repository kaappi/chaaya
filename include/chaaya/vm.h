#ifndef CHAAYA_VM_H
#define CHAAYA_VM_H

#include "chaaya/gc.h"
#include "chaaya/value.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CH_VM_MAX_FRAMES 256
#define CH_VM_MAX_REGS 4096
#define CH_VM_MAX_GLOBALS 1024
#define CH_VM_MAX_LIB_PATHS 32
#define CH_VM_MAX_SCRIPT_ARGS 64

typedef enum ChVMStatus {
    CH_VM_OK = 0,
    CH_VM_RUNTIME_ERROR,
    CH_VM_STACK_OVERFLOW,
} ChVMStatus;

typedef struct ChCallFrame {
    ChClosure *closure;
    uint8_t *ip;
    size_t reg_base; /* index into vm->regs */
    uint8_t num_regs;
} ChCallFrame;

typedef struct ChGlobal {
    ChSymbol *name;
    ChValue value;
    bool defined;
} ChGlobal;

typedef struct ChVM {
    ChGC gc;
    ChValue regs[CH_VM_MAX_REGS];
    size_t reg_top;
    ChCallFrame frames[CH_VM_MAX_FRAMES];
    size_t frame_count;
    ChGlobal globals[CH_VM_MAX_GLOBALS];
    size_t global_count;
    ChUpvalue *open_upvalues;
    ChValue result;
    char error[256];
    /* CLI / script context (owned strings are not strdup'd — point at argv) */
    const char *lib_paths[CH_VM_MAX_LIB_PATHS];
    size_t lib_path_count;
    const char *script_path;
    const char *script_args[CH_VM_MAX_SCRIPT_ARGS];
    size_t script_arg_count;
} ChVM;

void ch_vm_init(ChVM *vm);
void ch_vm_deinit(ChVM *vm);

int ch_vm_intern_global(ChVM *vm, ChSymbol *sym);
void ch_vm_define_global(ChVM *vm, int idx, ChValue v);

void ch_vm_register_primitives(ChVM *vm);

ChVMStatus ch_vm_call_closure(ChVM *vm, ChValue closure, ChValue *args, int nargs, ChValue *out);
ChVMStatus ch_vm_eval_function(ChVM *vm, ChFunction *fn, ChValue *out);

const char *ch_vm_error(const ChVM *vm);

#ifdef __cplusplus
}
#endif

#endif /* CHAAYA_VM_H */
