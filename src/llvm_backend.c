#include "chaaya/llvm_backend.h"

#include "chaaya/cli.h"

#include <stdio.h>

int ch_llvm_backend_run_file(const char *path) {
    if (!path) {
        path = "<stdin>";
    }
    fprintf(stderr,
            "chaaya: --native requested for '%s', but the LLVM backend MVP is not implemented yet.\n",
            path);
    return CH_EXIT_ERROR;
}
