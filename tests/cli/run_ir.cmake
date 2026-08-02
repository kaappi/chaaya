execute_process(
  COMMAND ${CHAAYA} ir ${INPUT}
  RESULT_VARIABLE rc
  OUTPUT_VARIABLE out
  ERROR_VARIABLE err
)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "ir failed: ${out}${err}")
endif()
if(NOT out MATCHES "\\(literal|\\(define|\\(call|\\(prim|\\(raw")
  message(FATAL_ERROR "expected IR dump, got: ${out}")
endif()
