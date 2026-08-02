#ifndef CHAAYA_PRIM_H
#define CHAAYA_PRIM_H

#include "chaaya/vm.h"

#ifdef __cplusplus
extern "C" {
#endif

void ch_register_core_primitives(ChVM *vm);
void ch_register_control_primitives(ChVM *vm);
/* Install Scheme dynamic-wind (call after error primitives are registered). */
void ch_install_control_bootstrap(ChVM *vm);
/* Remove %push-wind / %pop-wind / %wind-top-after from the global namespace. */
void ch_hide_control_internal_helpers(ChVM *vm);
void ch_register_list_primitives(ChVM *vm);
/* Overwrite map/for-each with Scheme implementations (call after error). */
void ch_install_list_bootstrap(ChVM *vm);
/* Overwrite vector-map / vector-for-each with Scheme (trampoline polish). */
void ch_install_vector_bootstrap(ChVM *vm);
/* Overwrite string-map / string-for-each with Scheme (trampoline polish). */
void ch_install_string_bootstrap(ChVM *vm);
void ch_register_data_primitives(ChVM *vm);
void ch_register_char_primitives(ChVM *vm);
void ch_register_string_primitives(ChVM *vm);
void ch_register_vector_primitives(ChVM *vm);
void ch_register_port_primitives(ChVM *vm);
void ch_register_record_primitives(ChVM *vm);
void ch_register_lazy_primitives(ChVM *vm);
void ch_register_math_primitives(ChVM *vm);
void ch_register_eval_primitives(ChVM *vm);
void ch_register_process_primitives(ChVM *vm);
void ch_register_error_primitives(ChVM *vm);
void ch_register_hashtable_primitives(ChVM *vm);
void ch_register_bytevector_primitives(ChVM *vm);
void ch_register_fiber_primitives(ChVM *vm);
void ch_register_ffi_primitives(ChVM *vm);
void ch_register_random_primitives(ChVM *vm);
void ch_register_filesystem_primitives(ChVM *vm);
void ch_register_weak_primitives(ChVM *vm);
void ch_register_srfi1_primitives(ChVM *vm);
void ch_register_srfi13_primitives(ChVM *vm);
void ch_register_srfi133_primitives(ChVM *vm);
void ch_register_srfi258_primitives(ChVM *vm);
void ch_register_srfi260_primitives(ChVM *vm);

#ifdef __cplusplus
}
#endif

#endif /* CHAAYA_PRIM_H */
