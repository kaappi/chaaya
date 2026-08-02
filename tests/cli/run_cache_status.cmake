execute_process(
  COMMAND ${CMAKE_COMMAND} -E env CHAAYA_HOME=${CMAKE_BINARY_DIR}/chaaya-home-test
          ${CHAAYA} cache status
  RESULT_VARIABLE rc
  OUTPUT_VARIABLE out
  ERROR_VARIABLE err
)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "cache status failed: ${out}${err}")
endif()
if(NOT out MATCHES "Bytecode cache")
  message(FATAL_ERROR "expected cache status header, got: ${out}")
endif()
