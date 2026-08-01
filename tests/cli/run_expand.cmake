execute_process(
  COMMAND ${CHAAYA} expand ${INPUT}
  RESULT_VARIABLE rc
  OUTPUT_VARIABLE out
  ERROR_VARIABLE err
)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "expand failed (${rc}): ${err}")
endif()
if(NOT out MATCHES "\\(if #t \\(begin 1 2\\) #f\\)")
  message(FATAL_ERROR "unexpected expand output: ${out}")
endif()
