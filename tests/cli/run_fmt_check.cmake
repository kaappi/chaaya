execute_process(
  COMMAND ${CHAAYA} fmt --check ${INPUT}
  RESULT_VARIABLE rc
  OUTPUT_VARIABLE out
  ERROR_VARIABLE err
)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "fmt --check failed (${rc}): ${err}${out}")
endif()
if(NOT out MATCHES "fmt: ok")
  message(FATAL_ERROR "unexpected fmt output: ${out}")
endif()
