execute_process(
  COMMAND ${CHAAYA} wasm
  RESULT_VARIABLE rc
  OUTPUT_VARIABLE out
  ERROR_VARIABLE err
)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "expected wasm helper command to succeed, got rc=${rc}: ${out}${err}")
endif()
set(combined "${out}${err}")
if(NOT combined MATCHES "build-wasm\\.sh|build-wasm|chaaya-wasm")
  message(FATAL_ERROR "expected wasm helper output/instructions, got: ${combined}")
endif()
