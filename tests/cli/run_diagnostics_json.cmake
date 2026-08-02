execute_process(
  COMMAND ${CHAAYA} check ${INPUT} --diagnostics=json
  RESULT_VARIABLE rc
  OUTPUT_VARIABLE out
  ERROR_VARIABLE err
)
if(rc EQUAL 0)
  message(FATAL_ERROR "expected check failure")
endif()
set(combined "${out}${err}")
if(NOT combined MATCHES "\"code\"")
  message(FATAL_ERROR "expected JSON diagnostic, got: ${combined}")
endif()
if(NOT combined MATCHES "CH[0-9][0-9][0-9][0-9]")
  message(FATAL_ERROR "expected CH code in JSON, got: ${combined}")
endif()
