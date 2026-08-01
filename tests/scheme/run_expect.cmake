# Compare stdout of `chaaya <input>` to an expected file (exact match).
if(NOT DEFINED CHAAYA OR NOT DEFINED INPUT OR NOT DEFINED EXPECTED)
  message(FATAL_ERROR "CHAAYA, INPUT, and EXPECTED must be set")
endif()

execute_process(
  COMMAND "${CHAAYA}" "${INPUT}"
  RESULT_VARIABLE rc
  OUTPUT_VARIABLE out
  ERROR_VARIABLE err
)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "chaaya failed (${rc}) on ${INPUT}:\n${err}")
endif()

file(READ "${EXPECTED}" expected)
if(NOT out STREQUAL expected)
  message(FATAL_ERROR
    "output mismatch for ${INPUT}\n--- got ---\n${out}\n--- expected ---\n${expected}")
endif()
