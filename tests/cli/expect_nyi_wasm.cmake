execute_process(
  COMMAND ${CHAAYA} wasm
  RESULT_VARIABLE rc
  OUTPUT_VARIABLE out
  ERROR_VARIABLE err
)
if(rc EQUAL 0)
  message(FATAL_ERROR "expected non-zero exit for wasm backend stub")
endif()
set(combined "${out}${err}")
if(NOT combined MATCHES "wasm.*not implemented")
  message(FATAL_ERROR "expected wasm stub message, got: ${combined}")
endif()
