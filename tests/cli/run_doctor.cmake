execute_process(
  COMMAND ${CHAAYA} doctor
  RESULT_VARIABLE rc
  OUTPUT_VARIABLE out
  ERROR_VARIABLE err
)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "doctor failed: ${out}${err}")
endif()
if(NOT out MATCHES "PASS|OK|chaaya|binary" AND NOT out MATCHES ".")
  message(FATAL_ERROR "unexpected empty doctor output")
endif()
