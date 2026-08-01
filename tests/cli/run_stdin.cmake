# Run (+ 1 2) on stdin through chaaya; expect exit 0.
execute_process(
  COMMAND ${CHAAYA}
  INPUT_FILE ${CMAKE_CURRENT_LIST_DIR}/stdin_add.scm
  RESULT_VARIABLE rc
  OUTPUT_VARIABLE out
  ERROR_VARIABLE err
)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "stdin eval failed (${rc}): ${err}")
endif()
