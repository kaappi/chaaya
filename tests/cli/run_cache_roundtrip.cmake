# Compile-cache round-trip: first run stores .chbc; second run loads it.
if(NOT DEFINED CHAAYA OR NOT DEFINED INPUT)
  message(FATAL_ERROR "CHAAYA and INPUT must be set")
endif()

set(home "${CMAKE_BINARY_DIR}/chaaya-cache-roundtrip-home")
file(REMOVE_RECURSE "${home}")
file(MAKE_DIRECTORY "${home}")

execute_process(
  COMMAND ${CMAKE_COMMAND} -E env "CHAAYA_HOME=${home}" ${CHAAYA} ${INPUT}
  RESULT_VARIABLE rc1
  OUTPUT_VARIABLE out1
  ERROR_VARIABLE err1
)
if(NOT rc1 EQUAL 0)
  message(FATAL_ERROR "cache round-trip store run failed (${rc1}): ${err1}\n${out1}")
endif()

file(GLOB chbc_files "${home}/cache/*.chbc")
list(LENGTH chbc_files nchbc)
if(nchbc EQUAL 0)
  message(FATAL_ERROR "expected a .chbc cache entry under ${home}/cache")
endif()

execute_process(
  COMMAND ${CMAKE_COMMAND} -E env "CHAAYA_HOME=${home}" ${CHAAYA} ${INPUT}
  RESULT_VARIABLE rc2
  OUTPUT_VARIABLE out2
  ERROR_VARIABLE err2
)
if(NOT rc2 EQUAL 0)
  message(FATAL_ERROR "cache round-trip load run failed (${rc2}): ${err2}\n${out2}")
endif()

if(NOT out1 STREQUAL out2)
  message(FATAL_ERROR
    "cache hit changed program output.\n--- store ---\n${out1}\n--- load ---\n${out2}")
endif()
