execute_process(
  COMMAND ${CHAAYA} --native ${INPUT}
  RESULT_VARIABLE rc
  OUTPUT_VARIABLE out
  ERROR_VARIABLE err
)
set(combined "${out}${err}")
# native_ok.scm is (+ 1 2) → constant-exit @main returns i32 3.
if(NOT rc EQUAL 3)
  message(FATAL_ERROR "expected --native exit code 3 for (+ 1 2), got rc=${rc}: ${combined}")
endif()
if(NOT combined MATCHES "compile: wrote")
  message(FATAL_ERROR "expected native compile message, got: ${combined}")
endif()
