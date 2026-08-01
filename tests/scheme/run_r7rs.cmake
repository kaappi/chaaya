if(NOT DEFINED CHAAYA OR NOT DEFINED INPUT OR NOT DEFINED LIB_PATH)
  message(FATAL_ERROR "CHAAYA, INPUT, and LIB_PATH must be set")
endif()

get_filename_component(input_dir "${INPUT}" DIRECTORY)

execute_process(
  COMMAND "${CHAAYA}" "--lib-path" "${LIB_PATH}" "${INPUT}"
  WORKING_DIRECTORY "${input_dir}"
  RESULT_VARIABLE rc
  OUTPUT_VARIABLE out
  ERROR_VARIABLE err
)

set(combined "${out}\n${err}")
string(REGEX MATCHALL "([0-9]+) pass, ([0-9]+) fail" section_summaries "${combined}")
list(LENGTH section_summaries section_count)

if(section_count EQUAL 0)
  message(FATAL_ERROR
    "r7rs suite did not report section pass/fail counts.\n"
    "--- stdout ---\n${out}\n--- stderr ---\n${err}")
endif()

set(total_pass 0)
set(total_fail 0)
foreach(summary IN LISTS section_summaries)
  string(REGEX REPLACE "^([0-9]+) pass, ([0-9]+) fail$" "\\1;\\2" fields "${summary}")
  list(GET fields 0 section_pass)
  list(GET fields 1 section_fail)
  math(EXPR total_pass "${total_pass} + ${section_pass}")
  math(EXPR total_fail "${total_fail} + ${section_fail}")
endforeach()

if(NOT rc EQUAL 0)
  string(REGEX MATCH "error:[^\n]*" first_error "${combined}")
  if(first_error STREQUAL "")
    set(first_error "no explicit error line")
  endif()
  message(FATAL_ERROR
    "r7rs suite failed with exit ${rc}: ${section_count} sections, "
    "${total_pass} pass, ${total_fail} fail, ${first_error}\n"
    "--- stdout ---\n${out}\n--- stderr ---\n${err}")
endif()

if(total_fail GREATER 0)
  message(FATAL_ERROR
    "r7rs suite reported failures: ${section_count} sections, "
    "${total_pass} pass, ${total_fail} fail\n"
    "--- stdout ---\n${out}\n--- stderr ---\n${err}")
endif()

message(STATUS
  "r7rs suite passed: ${section_count} sections, ${total_pass} pass, ${total_fail} fail")
