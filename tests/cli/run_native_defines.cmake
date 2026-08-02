# Compiles a program with defines via eval-fallback and runs it.
execute_process(
  COMMAND ${CHAAYA} compile ${INPUT} -o ${OUTPUT}
  RESULT_VARIABLE rc
  OUTPUT_VARIABLE out
  ERROR_VARIABLE err
)
set(combined "${out}${err}")
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "expected compile to succeed, got rc=${rc}: ${combined}")
endif()
execute_process(
  COMMAND ${OUTPUT}
  RESULT_VARIABLE run_rc
  OUTPUT_VARIABLE run_out
  ERROR_VARIABLE run_err
)
if(NOT run_rc EQUAL 0)
  message(FATAL_ERROR "expected native binary to exit 0, got rc=${run_rc}: ${run_out}${run_err}")
endif()
