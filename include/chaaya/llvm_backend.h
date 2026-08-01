#ifndef CHAAYA_LLVM_BACKEND_H
#define CHAAYA_LLVM_BACKEND_H

#ifdef __cplusplus
extern "C" {
#endif

/* Run a file via the LLVM/native backend path (MVP stub). */
int ch_llvm_backend_run_file(const char *path);

#ifdef __cplusplus
}
#endif

#endif /* CHAAYA_LLVM_BACKEND_H */
