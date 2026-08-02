#ifndef CHAAYA_DISASM_H
#define CHAAYA_DISASM_H

#include "chaaya/value.h"

#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

void ch_disassemble_function(FILE *out, ChFunction *fn);

#ifdef __cplusplus
}
#endif

#endif /* CHAAYA_DISASM_H */
