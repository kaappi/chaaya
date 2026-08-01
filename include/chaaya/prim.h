#ifndef CHAAYA_PRIM_H
#define CHAAYA_PRIM_H

#include "chaaya/vm.h"

#ifdef __cplusplus
extern "C" {
#endif

void ch_register_core_primitives(ChVM *vm);
void ch_register_control_primitives(ChVM *vm);
void ch_register_list_primitives(ChVM *vm);
void ch_register_data_primitives(ChVM *vm);
void ch_register_port_primitives(ChVM *vm);
void ch_register_record_primitives(ChVM *vm);

#ifdef __cplusplus
}
#endif

#endif /* CHAAYA_PRIM_H */
