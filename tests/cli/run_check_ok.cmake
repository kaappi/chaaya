execute_process(
  COMMAND ${CHAAYA} check ${INPUT}
  RESULT_VARIABLE rc
  OUTPUT_VARIABLE out
  ERROR_VARIABLE err
)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "check failed (${rc}): ${err}")
endif()
if(NOT out MATCHES "check: ok")
  message(FATAL_ERROR "missing success output from check: ${out}")
endif()
