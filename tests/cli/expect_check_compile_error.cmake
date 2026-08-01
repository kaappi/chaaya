execute_process(
  COMMAND ${CHAAYA} check ${INPUT}
  RESULT_VARIABLE rc
  OUTPUT_VARIABLE out
  ERROR_VARIABLE err
)
if(rc EQUAL 0)
  message(FATAL_ERROR "expected non-zero exit for check compile error")
endif()
set(combined "${out}${err}")
if(NOT combined MATCHES "check error:")
  message(FATAL_ERROR "expected check error diagnostics, got: ${combined}")
endif()
