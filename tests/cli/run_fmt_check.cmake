if(NOT DEFINED CHAAYA)
  message(FATAL_ERROR "CHAAYA must be set")
endif()

get_filename_component(FMT_TEST_DIR ${CMAKE_CURRENT_LIST_DIR} ABSOLUTE)

# Already-formatted file should pass --check.
if(NOT DEFINED INPUT)
  set(INPUT ${FMT_TEST_DIR}/fmt_ok.scm)
endif()

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

# Comment-preserving round-trip: format to temp, verify comment text survives.
set(COMMENTS_FILE ${FMT_TEST_DIR}/fmt_comments.scm)
set(TEMP_OUT ${CMAKE_BINARY_DIR}/fmt_comments_out.scm)

execute_process(
  COMMAND ${CHAAYA} fmt -o ${TEMP_OUT} ${COMMENTS_FILE}
  RESULT_VARIABLE rc2
  OUTPUT_VARIABLE out2
  ERROR_VARIABLE err2
)
if(NOT rc2 EQUAL 0)
  message(FATAL_ERROR "fmt write failed (${rc2}): ${err2}${out2}")
endif()

file(READ ${TEMP_OUT} formatted)
if(NOT formatted MATCHES "; keep me")
  message(FATAL_ERROR "formatted output lost trailing comment: ${formatted}")
endif()

execute_process(
  COMMAND ${CHAAYA} fmt --check ${TEMP_OUT}
  RESULT_VARIABLE rc3
  OUTPUT_VARIABLE out3
  ERROR_VARIABLE err3
)
if(NOT rc3 EQUAL 0)
  message(FATAL_ERROR "formatted temp file failed --check (${rc3}): ${err3}${out3}")
endif()
if(NOT out3 MATCHES "fmt: ok")
  message(FATAL_ERROR "formatted temp file not idempotent: ${out3}")
endif()
