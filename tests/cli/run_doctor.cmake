execute_process(
  COMMAND ${CHAAYA} doctor
  RESULT_VARIABLE rc
  OUTPUT_VARIABLE out
  ERROR_VARIABLE err
)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "doctor failed: ${out}${err}")
endif()
if(NOT out MATCHES "PASS|FAIL|WARN|binary|==")
  message(FATAL_ERROR "unexpected doctor text output: ${out}${err}")
endif()

execute_process(
  COMMAND ${CHAAYA} doctor --json
  RESULT_VARIABLE rc_json
  OUTPUT_VARIABLE out_json
  ERROR_VARIABLE err_json
)
if(NOT rc_json EQUAL 0)
  message(FATAL_ERROR "doctor --json failed: ${out_json}${err_json}")
endif()
if(NOT out_json MATCHES "\"ok\"" AND NOT out_json MATCHES "\"checks\"")
  message(FATAL_ERROR "doctor --json missing ok/checks: ${out_json}${err_json}")
endif()
