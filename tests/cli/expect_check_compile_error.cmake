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
if(NOT combined MATCHES "CH[0-9][0-9][0-9][0-9]")
  message(FATAL_ERROR "expected CH diagnostic code, got: ${combined}")
endif()
