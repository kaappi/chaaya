#ifndef CHAAYA_PRINTER_H
#define CHAAYA_PRINTER_H

#include "chaaya/value.h"

#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* write mode: #t/#f, quoted strings/symbols as needed.
 * display mode: strings without quotes, etc. */
void ch_print_value(FILE *out, ChValue v, bool display);
char *ch_value_to_string(ChValue v, bool display); /* malloc'd; caller frees */

#ifdef __cplusplus
}
#endif

#endif /* CHAAYA_PRINTER_H */
