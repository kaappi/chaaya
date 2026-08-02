if(NOT DEFINED CHAAYA OR NOT DEFINED INPUT OR NOT DEFINED LIB_PATH)
  message(FATAL_ERROR "CHAAYA, INPUT, and LIB_PATH must be set")
endif()

get_filename_component(input_dir "${INPUT}" DIRECTORY)
get_filename_component(input_name "${INPUT}" NAME)

# Isolate bytecode cache from the developer home directory.
set(test_home "${CMAKE_BINARY_DIR}/chaaya-deferred-home")
file(MAKE_DIRECTORY "${test_home}")

execute_process(
  COMMAND ${CMAKE_COMMAND} -E env
    "CHAAYA_HOME=${test_home}"
    "CHAAYA_NO_CACHE=1"
    "${CHAAYA}" "--lib-path" "${LIB_PATH}" "--lib-path" "${input_dir}" "${input_name}"
  WORKING_DIRECTORY "${input_dir}"
  RESULT_VARIABLE rc
  OUTPUT_VARIABLE out
  ERROR_VARIABLE err
)

set(combined "${out}\n${err}")

# SRFI-64 style: (N pass, M fail) section summaries
string(REGEX MATCHALL "([0-9]+) pass, ([0-9]+) fail" section_summaries "${combined}")
list(LENGTH section_summaries section_count)

set(total_pass 0)
set(total_fail 0)
foreach(summary IN LISTS section_summaries)
  string(REGEX REPLACE "^([0-9]+) pass, ([0-9]+) fail$" "\\1;\\2" fields "${summary}")
  list(GET fields 0 section_pass)
  list(GET fields 1 section_fail)
  math(EXPR total_pass "${total_pass} + ${section_pass}")
  math(EXPR total_fail "${total_fail} + ${section_fail}")
endforeach()

# Some deferred files exit 1 with %test-fail-count but no section summaries
if(section_count EQUAL 0)
  string(REGEX MATCH "FAIL" fail_marker "${combined}")
  if(NOT fail_marker STREQUAL "")
    message(FATAL_ERROR
      "kaappi-deferred suite reported FAIL without pass/fail summary.\n"
      "--- stdout ---\n${out}\n--- stderr ---\n${err}")
  endif()
endif()

if(NOT rc EQUAL 0)
  string(REGEX MATCH "error:[^\n]*" first_error "${combined}")
  if(first_error STREQUAL "")
    set(first_error "non-zero exit without error line")
  endif()
  message(FATAL_ERROR
    "kaappi-deferred suite failed with exit ${rc}: "
    "${total_pass} pass, ${total_fail} fail, ${first_error}\n"
    "--- stdout ---\n${out}\n--- stderr ---\n${err}")
endif()

if(total_fail GREATER 0)
  message(FATAL_ERROR
    "kaappi-deferred suite reported failures: "
    "${total_pass} pass, ${total_fail} fail\n"
    "--- stdout ---\n${out}\n--- stderr ---\n${err}")
endif()

message(STATUS
  "kaappi-deferred passed: ${section_count} sections, ${total_pass} pass, ${total_fail} fail")
