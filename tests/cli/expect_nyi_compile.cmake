execute_process(
  COMMAND ${CHAAYA} compile x.scm
  RESULT_VARIABLE rc
  OUTPUT_VARIABLE out
  ERROR_VARIABLE err
)
if(rc EQUAL 0)
  message(FATAL_ERROR "expected non-zero exit for compile")
endif()
set(combined "${out}${err}")
if(NOT combined MATCHES "not implemented")
  message(FATAL_ERROR "expected not-implemented message, got: ${combined}")
endif()
