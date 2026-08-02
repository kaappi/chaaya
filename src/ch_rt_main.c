#include <stdint.h>

/*
 * Runtime fallback for LLVM MVP lowering.
 *
 * When the source cannot be reduced to a compile-time constant exit code,
 * generated IR calls this symbol.
 */
int ch_rt_main(void) {
    return 0;
}
