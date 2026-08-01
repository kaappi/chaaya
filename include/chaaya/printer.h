#ifndef CHAAYA_PRINTER_H
#define CHAAYA_PRINTER_H

#include "chaaya/value.h"

#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum ChPrintMode {
    CH_PRINT_WRITE = 0,   /* machine-readable; datum labels for cycles only */
    CH_PRINT_DISPLAY,     /* human-readable; datum labels for cycles only */
    CH_PRINT_SHARED,      /* like write, labels all shared structure */
    CH_PRINT_SIMPLE,      /* like write, never uses datum labels */
} ChPrintMode;

void ch_print_value_mode(FILE *out, ChValue v, ChPrintMode mode);
char *ch_value_to_string_mode(ChValue v, ChPrintMode mode); /* malloc'd; caller frees */

/* Convenience wrappers (write / display). */
void ch_print_value(FILE *out, ChValue v, bool display);
char *ch_value_to_string(ChValue v, bool display);

#ifdef __cplusplus
}
#endif

#endif /* CHAAYA_PRINTER_H */
