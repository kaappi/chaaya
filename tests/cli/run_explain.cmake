execute_process(
  COMMAND ${CHAAYA} explain CH3001
  RESULT_VARIABLE rc
  OUTPUT_VARIABLE out
  ERROR_VARIABLE err
)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "explain failed: ${out}${err}")
endif()
if(NOT out MATCHES "CH3001")
  message(FATAL_ERROR "expected CH3001 in explain output, got: ${out}")
endif()
if(NOT out MATCHES "undefined-variable")
  message(FATAL_ERROR "expected name in explain output, got: ${out}")
endif()
