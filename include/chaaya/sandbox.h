#ifndef CHAAYA_SANDBOX_H
#define CHAAYA_SANDBOX_H

#ifdef __cplusplus
extern "C" {
#endif

/* When enabled, restrict filesystem opens outside cwd, process spawn, FFI, eval/load. */
void ch_sandbox_enable(void);
int ch_sandbox_enabled(void);

/* Return 1 if the operation is denied (caller should raise an error). */
int ch_sandbox_deny_fs(const char *path);
int ch_sandbox_deny_process(void);
int ch_sandbox_deny_ffi(void);
int ch_sandbox_deny_eval(void);

#ifdef __cplusplus
}
#endif

#endif /* CHAAYA_SANDBOX_H */
