#ifndef CHAAYA_LLVM_BACKEND_H
#define CHAAYA_LLVM_BACKEND_H

#ifdef __cplusplus
extern "C" {
#endif

/* Run a file via the LLVM/native backend path. */
int ch_llvm_backend_run_file(const char *path);

/* Emit LLVM IR text for a Scheme file to path or stdout (NULL out_path). */
int ch_llvm_backend_emit_ir(const char *path, const char *out_path);

/* Compile to a native binary via clang/cc when available. */
int ch_llvm_backend_compile_native(const char *path, const char *out_path);

#ifdef __cplusplus
}
#endif

#endif /* CHAAYA_LLVM_BACKEND_H */
