execute_process(
  COMMAND ${CHAAYA} --native ${INPUT}
  RESULT_VARIABLE rc
  OUTPUT_VARIABLE out
  ERROR_VARIABLE err
)
if(rc EQUAL 0)
  message(FATAL_ERROR "expected non-zero exit for --native backend stub")
endif()
set(combined "${out}${err}")
if(NOT combined MATCHES "LLVM backend MVP is not implemented")
  message(FATAL_ERROR "expected native backend stub message, got: ${combined}")
endif()
