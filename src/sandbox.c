#include "chaaya/sandbox.h"

#include <string.h>

static int g_enabled;

void ch_sandbox_enable(void) {
    g_enabled = 1;
}

int ch_sandbox_enabled(void) {
    return g_enabled;
}

int ch_sandbox_deny_fs(const char *path) {
    if (!g_enabled || !path) {
        return 0;
    }
    /* Deny absolute paths and parent escapes outside the working tree. */
    if (path[0] == '/') {
        return 1;
    }
    if (strstr(path, "..") != NULL) {
        return 1;
    }
    return 0;
}

int ch_sandbox_deny_process(void) {
    return g_enabled;
}

int ch_sandbox_deny_ffi(void) {
    return g_enabled;
}

int ch_sandbox_deny_eval(void) {
    return g_enabled;
}
