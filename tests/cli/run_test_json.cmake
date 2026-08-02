execute_process(
  COMMAND ${CHAAYA} test --json --lib-path ${LIB_PATH} ${PASS_FILE}
  RESULT_VARIABLE pass_rc
  OUTPUT_VARIABLE pass_out
  ERROR_VARIABLE pass_err
)
if(NOT pass_rc EQUAL 0)
  message(FATAL_ERROR "expected pass suite to succeed:\n${pass_out}${pass_err}")
endif()
if(NOT pass_out MATCHES "\"type\":\"file\"")
  message(FATAL_ERROR "missing file JSON object:\n${pass_out}")
endif()
if(NOT pass_out MATCHES "\"type\":\"summary\"")
  message(FATAL_ERROR "missing summary JSON object:\n${pass_out}")
endif()
if(NOT pass_out MATCHES "\"pass\":1")
  message(FATAL_ERROR "expected pass:1 in JSON:\n${pass_out}")
endif()

execute_process(
  COMMAND ${CHAAYA} test --json --lib-path ${LIB_PATH} ${FAIL_FILE}
  RESULT_VARIABLE fail_rc
  OUTPUT_VARIABLE fail_out
  ERROR_VARIABLE fail_err
)
if(fail_rc EQUAL 0)
  message(FATAL_ERROR "expected fail suite to exit nonzero:\n${fail_out}${fail_err}")
endif()
if(NOT fail_out MATCHES "\"fail\":1" AND NOT fail_out MATCHES "\"error\":true")
  message(FATAL_ERROR "expected fail/error in JSON:\n${fail_out}")
endif()
if(NOT fail_out MATCHES "intentional-fail" AND NOT fail_out MATCHES "\"failures\"")
  message(FATAL_ERROR "expected structured failure detail:\n${fail_out}")
endif()
