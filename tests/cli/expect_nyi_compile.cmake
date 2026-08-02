execute_process(
  COMMAND ${CHAAYA} compile ${INPUT} -o ${OUTPUT}
  RESULT_VARIABLE rc
  OUTPUT_VARIABLE out
  ERROR_VARIABLE err
)
set(combined "${out}${err}")
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "expected compile to succeed (MVP), got rc=${rc}: ${combined}")
endif()
if(NOT combined MATCHES "compile: wrote")
  message(FATAL_ERROR "expected compile success message, got: ${combined}")
endif()
