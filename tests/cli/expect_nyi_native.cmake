execute_process(
  COMMAND ${CHAAYA} --native ${INPUT}
  RESULT_VARIABLE rc
  OUTPUT_VARIABLE out
  ERROR_VARIABLE err
)
set(combined "${out}${err}")
# MVP native path should compile/link and run successfully.
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "expected --native MVP to succeed, got rc=${rc}: ${combined}")
endif()
if(NOT combined MATCHES "compile: wrote")
  message(FATAL_ERROR "expected native compile message, got: ${combined}")
endif()
