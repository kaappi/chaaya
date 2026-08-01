# Run a bootstrap Scheme suite: concatenate harness.scm + INPUT, then run chaaya.
# Expects exit code 0 (check-finish calls exit).
# Optional: LIB_PATH — passed as --lib-path to chaaya.
# Working directory is the INPUT file's directory so relative include paths work.
if(NOT DEFINED CHAAYA OR NOT DEFINED HARNESS OR NOT DEFINED INPUT)
  message(FATAL_ERROR "CHAAYA, HARNESS, and INPUT must be set")
endif()

file(READ "${HARNESS}" harness)
file(READ "${INPUT}" body)
set(combined "${harness}\n${body}")

get_filename_component(input_dir "${INPUT}" DIRECTORY)

string(RANDOM LENGTH 8 ALPHABET "abcdefghijklmnopqrstuvwxyz0123456789" rnd)
set(tmp "${input_dir}/.bootstrap_${rnd}.scm")
file(WRITE "${tmp}" "${combined}")

if(DEFINED LIB_PATH)
  execute_process(
    COMMAND "${CHAAYA}" "--lib-path" "${LIB_PATH}" "${tmp}"
    WORKING_DIRECTORY "${input_dir}"
    RESULT_VARIABLE rc
    OUTPUT_VARIABLE out
    ERROR_VARIABLE err
  )
else()
  execute_process(
    COMMAND "${CHAAYA}" "${tmp}"
    WORKING_DIRECTORY "${input_dir}"
    RESULT_VARIABLE rc
    OUTPUT_VARIABLE out
    ERROR_VARIABLE err
  )
endif()
file(REMOVE "${tmp}")

if(NOT rc EQUAL 0)
  message(FATAL_ERROR
    "bootstrap suite failed (${rc}): ${INPUT}\n--- stdout ---\n${out}\n--- stderr ---\n${err}")
endif()

message(STATUS "bootstrap ok: ${INPUT}\n${out}")
