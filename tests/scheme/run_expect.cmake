# Compare stdout of `chaaya <input>` to an expected file (exact match).
if(NOT DEFINED CHAAYA OR NOT DEFINED INPUT OR NOT DEFINED EXPECTED)
  message(FATAL_ERROR "CHAAYA, INPUT, and EXPECTED must be set")
endif()

# Isolate home + disable bytecode cache. Probe files exercise IR opts around
# redefining builtins; a warm ~/.chaaya/cache entry can reload bytecode that
# was compiled under a different global-binding snapshot and fail golden
# compares even when a fresh compile is correct.
get_filename_component(_expect_binary_dir "${CHAAYA}" DIRECTORY)
set(_probe_home "${_expect_binary_dir}/chaaya-probe-home")
file(MAKE_DIRECTORY "${_probe_home}")

execute_process(
  COMMAND ${CMAKE_COMMAND} -E env
    "CHAAYA_HOME=${_probe_home}"
    "CHAAYA_NO_CACHE=1"
    "${CHAAYA}" "${INPUT}"
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
