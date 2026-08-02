#include "chaaya/doctor.h"

#include "chaaya/ffi.h"
#include "chaaya/runtime_exports.h"
#include "chaaya/version.h"
#include "chaaya/vm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#ifndef CHAAYA_SOURCE_DIR
#define CHAAYA_SOURCE_DIR "."
#endif

#ifndef CHAAYA_BUILD_DIR
#define CHAAYA_BUILD_DIR "."
#endif

#define CH_DOCTOR_MAX_CHECKS 64

typedef struct DoctorCheck {
    const char *group;
    const char *status;
    const char *name;
    char detail[640];
    const char *suggestion;
} DoctorCheck;

typedef struct DoctorCtx {
    DoctorCheck checks[CH_DOCTOR_MAX_CHECKS];
    int count;
    int fails;
    int warns;
    int json;
    const char *last_text_group;
} DoctorCtx;

static int path_writable_dir(const char *dir) {
    struct stat st;
    if (stat(dir, &st) != 0) {
        return 0;
    }
    return S_ISDIR(st.st_mode) && access(dir, W_OK) == 0;
}

static int path_readable_dir(const char *dir) {
    struct stat st;
    if (stat(dir, &st) != 0) {
        return 0;
    }
    return S_ISDIR(st.st_mode) && access(dir, R_OK) == 0;
}

static int path_has_executable(const char *name) {
    const char *path_env = getenv("PATH");
    if (!path_env || !path_env[0]) {
        return 0;
    }
    char *copy = strdup(path_env);
    if (!copy) {
        return 0;
    }
    int found = 0;
    char *save = NULL;
    for (char *dir = strtok_r(copy, ":", &save); dir; dir = strtok_r(NULL, ":", &save)) {
        if (!dir[0]) {
            continue;
        }
        char probe[PATH_MAX];
        if (snprintf(probe, sizeof(probe), "%s/%s", dir, name) >= (int)sizeof(probe)) {
            continue;
        }
        if (access(probe, X_OK) == 0) {
            found = 1;
            break;
        }
    }
    free(copy);
    return found;
}

static int resolve_cache_dir(char *out, size_t out_len) {
    const char *base = getenv("CHAAYA_HOME");
    if (base && base[0] != '\0') {
        return snprintf(out, out_len, "%s/cache", base) < (int)out_len ? 0 : -1;
    }
    const char *home = getenv("HOME");
    if (!home || home[0] == '\0') {
        return -1;
    }
    return snprintf(out, out_len, "%s/.chaaya/cache", home) < (int)out_len ? 0 : -1;
}

static int resolve_executable_path(char *out, size_t out_len) {
#if defined(__linux__)
    ssize_t n = readlink("/proc/self/exe", out, out_len - 1);
    if (n > 0) {
        out[n] = '\0';
        return 0;
    }
#elif defined(__APPLE__)
    uint32_t size = (uint32_t)out_len;
    if (_NSGetExecutablePath(out, &size) == 0) {
        char resolved[PATH_MAX];
        if (realpath(out, resolved) != NULL) {
            if (snprintf(out, out_len, "%s", resolved) >= (int)out_len) {
                return -1;
            }
        }
        return 0;
    }
#endif
    if (snprintf(out, out_len, "(unknown)") >= (int)out_len) {
        return -1;
    }
    return -1;
}

static void json_escape(FILE *out, const char *s) {
    fputc('"', out);
    for (const char *p = s; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '"' || c == '\\') {
            fputc('\\', out);
            fputc((int)c, out);
        } else if (c < 0x20) {
            fprintf(out, "\\u%04x", c);
        } else {
            fputc((int)c, out);
        }
    }
    fputc('"', out);
}

static void doctor_add(DoctorCtx *ctx, const char *group, const char *status, const char *name,
                       const char *detail, const char *suggestion) {
    if (ctx->count >= CH_DOCTOR_MAX_CHECKS) {
        return;
    }
    DoctorCheck *c = &ctx->checks[ctx->count++];
    c->group = group;
    c->status = status;
    c->name = name;
    c->suggestion = suggestion;
    snprintf(c->detail, sizeof(c->detail), "%s", detail ? detail : "");

    if (strcmp(status, "FAIL") == 0) {
        ctx->fails++;
    } else if (strcmp(status, "WARN") == 0) {
        ctx->warns++;
    }

    if (!ctx->json) {
        if (!ctx->last_text_group || strcmp(ctx->last_text_group, group) != 0) {
            printf("\n== %s ==\n", group);
            ctx->last_text_group = group;
        }
        printf("%-4s %s — %s\n", status, name, c->detail);
        if (suggestion && suggestion[0]) {
            printf("     → %s\n", suggestion);
        }
    }
}

static const char *overall_status(const DoctorCtx *ctx) {
    if (ctx->fails > 0) {
        return "fail";
    }
    if (ctx->warns > 0) {
        return "warn";
    }
    return "pass";
}

static void doctor_emit_json(const DoctorCtx *ctx) {
    printf("{\n");
    printf("  \"ok\": %s,\n", ctx->fails == 0 ? "true" : "false");
    printf("  \"status\": ");
    json_escape(stdout, overall_status(ctx));
    printf(",\n  \"version\": ");
    json_escape(stdout, CHAAYA_VERSION);
    printf(",\n  \"checks\": [\n");
    for (int i = 0; i < ctx->count; i++) {
        const DoctorCheck *c = &ctx->checks[i];
        if (i > 0) {
            fputc(',', stdout);
        }
        fputc('\n', stdout);
        printf("    {\"group\": ");
        json_escape(stdout, c->group);
        printf(", \"status\": ");
        json_escape(stdout, c->status);
        printf(", \"name\": ");
        json_escape(stdout, c->name);
        printf(", \"detail\": ");
        json_escape(stdout, c->detail);
        printf(", \"suggestion\": ");
        if (c->suggestion && c->suggestion[0]) {
            json_escape(stdout, c->suggestion);
        } else {
            fputs("null", stdout);
        }
        fputc('}', stdout);
    }
    if (ctx->count > 0) {
        fputc('\n', stdout);
    }
    printf("  ]\n}\n");
}

static void doctor_check_binary(DoctorCtx *ctx) {
    doctor_add(ctx, "binary", "PASS", "version", CHAAYA_VERSION_BANNER, NULL);

    {
        char exe[PATH_MAX];
        if (resolve_executable_path(exe, sizeof(exe)) == 0) {
            doctor_add(ctx, "binary", "PASS", "executable", exe, NULL);
        } else {
            doctor_add(ctx, "binary", "WARN", "executable", exe,
                       "could not resolve running binary path on this platform");
        }
    }

    if (path_has_executable("chaaya")) {
        doctor_add(ctx, "binary", "PASS", "on-path", "chaaya found in PATH", NULL);
    } else {
        doctor_add(ctx, "binary", "WARN", "on-path", "chaaya not found in PATH",
                   "add the install or build directory to PATH for shell completion");
    }

#ifdef CHAAYA_HAS_LINENOISE
    doctor_add(ctx, "binary", "PASS", "linenoise", "built with linenoise", NULL);
#else
    doctor_add(ctx, "binary", "WARN", "linenoise", "built without linenoise (plain REPL)",
               "rebuild with CHAAYA_USE_LINENOISE=ON for line editing in the REPL");
#endif
}

static void doctor_check_library(DoctorCtx *ctx, const ChCliOptions *opts, const char *home) {
    for (size_t i = 0; i < opts->lib_path_count; i++) {
        char buf[640];
        snprintf(buf, sizeof(buf), "%s", opts->lib_paths[i]);
        if (path_readable_dir(opts->lib_paths[i]) || path_writable_dir(opts->lib_paths[i])) {
            doctor_add(ctx, "library", "PASS", "lib-path", buf, NULL);
        } else if (access(opts->lib_paths[i], R_OK) == 0) {
            doctor_add(ctx, "library", "PASS", "lib-path", buf, NULL);
        } else {
            doctor_add(ctx, "library", "WARN", "lib-path", buf,
                       "directory missing or not readable; pass --lib-path to a valid tree");
        }
    }

    {
        char user_lib[512];
        snprintf(user_lib, sizeof(user_lib), "%s/lib", home);
        if (path_readable_dir(user_lib)) {
            doctor_add(ctx, "library", "PASS", "user-lib", user_lib, NULL);
        } else if (opts->lib_path_count == 0) {
            doctor_add(ctx, "library", "WARN", "user-lib", user_lib,
                       "default library directory does not exist; use --lib-path or create it");
        }
        /* skip user-lib when explicit --lib-path entries are configured */
    }

    {
        char cache_dir[PATH_MAX];
        if (resolve_cache_dir(cache_dir, sizeof(cache_dir)) == 0) {
            if (path_writable_dir(cache_dir) || access(cache_dir, R_OK) == 0 ||
                access(cache_dir, F_OK) != 0) {
                doctor_add(ctx, "library", "PASS", "cache-dir", cache_dir, NULL);
            } else {
                doctor_add(ctx, "library", "WARN", "cache-dir", cache_dir,
                           "cache directory is not accessible");
            }
        } else {
            doctor_add(ctx, "library", "WARN", "cache-dir", "could not resolve cache directory",
                       "set HOME or CHAAYA_HOME");
        }
    }

    if (path_has_executable("thottam")) {
        doctor_add(ctx, "library", "PASS", "ecosystem", "thottam found in PATH", NULL);
    } else {
        doctor_add(ctx, "library", "WARN", "ecosystem",
                   "Kaappi thottam not in PATH (optional; use --lib-path for portable libs)",
                   NULL);
    }
}

static void doctor_check_repl(DoctorCtx *ctx, const char *home, const char *env_home) {
    if (getenv("HOME") && getenv("HOME")[0]) {
        if (env_home && env_home[0] && !path_writable_dir(home)) {
            doctor_add(ctx, "repl", "WARN", "chaaya_home", "CHAAYA_HOME is not a writable directory",
                       "create the directory or choose a writable CHAAYA_HOME");
        } else {
            doctor_add(ctx, "repl", "PASS", "chaaya_home", home, NULL);
        }
    } else {
        doctor_add(ctx, "repl", "FAIL", "chaaya_home", "HOME is unset",
                   "set HOME or CHAAYA_HOME for REPL history and cache");
    }

    if (isatty(STDIN_FILENO)) {
        doctor_add(ctx, "repl", "PASS", "stdin-tty", "stdin is a TTY", NULL);
    } else {
        doctor_add(ctx, "repl", "WARN", "stdin-tty", "stdin is not a TTY",
                   "interactive REPL features require a terminal on stdin");
    }

    if (isatty(STDOUT_FILENO)) {
        doctor_add(ctx, "repl", "PASS", "stdout-tty", "stdout is a TTY", NULL);
    } else {
        doctor_add(ctx, "repl", "WARN", "stdout-tty", "stdout is not a TTY", NULL);
    }

    {
        const char *term = getenv("TERM");
        if (isatty(STDOUT_FILENO) && (!term || !term[0])) {
            doctor_add(ctx, "repl", "WARN", "term", "TERM is unset",
                       "set TERM for terminal capability detection");
        } else if (term && term[0]) {
            doctor_add(ctx, "repl", "PASS", "term", term, NULL);
        } else {
            doctor_add(ctx, "repl", "PASS", "term", "not applicable (non-TTY)", NULL);
        }
    }
}

static void doctor_check_ffi(DoctorCtx *ctx) {
    ChVM probe_vm;
    ch_vm_init(&probe_vm);
#if defined(__APPLE__)
    const char *libc_probe = "libc.dylib";
#elif defined(__linux__)
    const char *libc_probe = "libc.so.6";
#else
    const char *libc_probe = NULL;
#endif
    if (!libc_probe) {
        doctor_add(ctx, "ffi", "WARN", "dlopen", "ffi probe skipped on this platform", NULL);
    } else {
        ChValue lib = CH_UNDEFINED;
        if (ch_ffi_open_library(&probe_vm, libc_probe, &lib) == 0) {
            (void)ch_ffi_close_library(&probe_vm, lib);
            doctor_add(ctx, "ffi", "PASS", "dlopen", libc_probe, NULL);
        } else {
            char detail[256];
            snprintf(detail, sizeof(detail), "%s", ch_vm_error(&probe_vm));
            doctor_add(ctx, "ffi", "WARN", "dlopen", detail,
                       "FFI requires a working dynamic linker on this platform");
        }
    }
    ch_vm_deinit(&probe_vm);
}

static void doctor_check_native_backend(DoctorCtx *ctx) {
    if (!ch_rt_native_arch_supported()) {
        doctor_add(ctx, "native-backend", "WARN", "architecture",
                   "unsupported architecture (aarch64/x86_64 only)", NULL);
        return;
    }

    const char *native_cc = getenv("CHAAYA_LLVM_CC");
    const char *cc = NULL;
    if (native_cc && native_cc[0] && path_has_executable(native_cc)) {
        cc = native_cc;
    } else if (path_has_executable("clang")) {
        cc = "clang";
    } else if (path_has_executable("cc")) {
        cc = "cc";
    }

    char rt_lib[PATH_MAX];
    int have_rt = 0;
    const char *lib_dir = getenv("CHAAYA_LIB_DIR");
    if (lib_dir && lib_dir[0]) {
        snprintf(rt_lib, sizeof(rt_lib), "%s/libchaaya_rt.a", lib_dir);
        have_rt = access(rt_lib, R_OK) == 0;
    }
    if (!have_rt) {
        snprintf(rt_lib, sizeof(rt_lib), "%s/libchaaya_rt.a", CHAAYA_BUILD_DIR);
        have_rt = access(rt_lib, R_OK) == 0;
    }
    if (!have_rt) {
        snprintf(rt_lib, sizeof(rt_lib), "%s/build/libchaaya_rt.a", CHAAYA_SOURCE_DIR);
        have_rt = access(rt_lib, R_OK) == 0;
    }

    if (!cc) {
        doctor_add(ctx, "native-backend", "WARN", "compiler", "no C/LLVM compiler in PATH",
                   "install clang or set CHAAYA_LLVM_CC for native compilation");
    } else if (!have_rt) {
        doctor_add(ctx, "native-backend", "WARN", "runtime",
                   "libchaaya_rt.a not found (build chaaya_rt or set CHAAYA_LIB_DIR)",
                   "run cmake --build build to produce libchaaya_rt.a");
    } else {
        char src_path[PATH_MAX];
        char bin_path[PATH_MAX];
        snprintf(src_path, sizeof(src_path), "/tmp/chaaya-doctor-rt-%d.c", (int)getpid());
        snprintf(bin_path, sizeof(bin_path), "/tmp/chaaya-doctor-rt-%d", (int)getpid());
        FILE *sf = fopen(src_path, "w");
        int smoke_ok = 0;
        if (sf) {
            fputs("#include <stdint.h>\n"
                  "uint64_t ch_rt_fixnum_add(uint64_t, uint64_t);\n"
                  "int main(void) {\n"
                  "  uint64_t a = 0xFFFD000000000001ULL;\n"
                  "  uint64_t b = 0xFFFD000000000002ULL;\n"
                  "  uint64_t r = ch_rt_fixnum_add(a, b);\n"
                  "  return (r == 0xFFFD000000000003ULL) ? 0 : 1;\n"
                  "}\n",
                  sf);
            fclose(sf);
            char cmd[2048];
#if defined(__APPLE__)
            snprintf(cmd, sizeof(cmd), "%s -O0 -o \"%s\" \"%s\" \"%s\"", cc, bin_path, src_path,
                     rt_lib);
#else
            snprintf(cmd, sizeof(cmd), "%s -O0 -o \"%s\" \"%s\" \"%s\" -lpthread -ldl -lm", cc,
                     bin_path, src_path, rt_lib);
#endif
            if (system(cmd) == 0) {
                int st = system(bin_path);
                smoke_ok = WIFEXITED(st) && WEXITSTATUS(st) == 0;
            }
            unlink(src_path);
            unlink(bin_path);
        }
        if (smoke_ok) {
            doctor_add(ctx, "native-backend", "PASS", "smoke-link", "libchaaya_rt smoke-link ok",
                       NULL);
        } else {
            doctor_add(ctx, "native-backend", "WARN", "smoke-link", "libchaaya_rt smoke-link failed",
                       "verify CHAAYA_LIB_DIR and compiler toolchain");
        }
    }
}

int ch_doctor_run(const ChCliOptions *opts) {
    DoctorCtx ctx = {.json = opts->json};

    char home[512];
    const char *env = getenv("CHAAYA_HOME");
    if (env && env[0]) {
        snprintf(home, sizeof(home), "%s", env);
    } else {
        const char *h = getenv("HOME");
        if (!h) {
            h = "";
        }
        snprintf(home, sizeof(home), "%s/.chaaya", h);
    }

    doctor_check_binary(&ctx);
    doctor_check_library(&ctx, opts, home);
    doctor_check_native_backend(&ctx);
    doctor_check_repl(&ctx, home, env);
    doctor_check_ffi(&ctx);

    if (ctx.json) {
        doctor_emit_json(&ctx);
    } else {
        printf("\n%d fail(s), %d warning(s) — overall %s\n", ctx.fails, ctx.warns,
               overall_status(&ctx));
    }

    return ctx.fails ? CH_EXIT_ERROR : CH_EXIT_OK;
}
