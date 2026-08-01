set(initialize "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{}}")
set(shutdown "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"shutdown\",\"params\":null}")
string(LENGTH "${initialize}" initialize_len)
string(LENGTH "${shutdown}" shutdown_len)

set(input_file "${CMAKE_CURRENT_BINARY_DIR}/lsp_initialize_shutdown.input")
file(WRITE "${input_file}"
  "Content-Length: ${initialize_len}\r\n\r\n${initialize}Content-Length: ${shutdown_len}\r\n\r\n${shutdown}"
)

execute_process(
  COMMAND ${CHAAYA} lsp
  INPUT_FILE "${input_file}"
  RESULT_VARIABLE rc
  OUTPUT_VARIABLE out
  ERROR_VARIABLE err
)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "lsp stub failed (${rc}): ${err}")
endif()
if(NOT out MATCHES "\"id\":1")
  message(FATAL_ERROR "missing initialize response id in output: ${out}")
endif()
if(NOT out MATCHES "\"capabilities\"")
  message(FATAL_ERROR "missing initialize capabilities in output: ${out}")
endif()
if(NOT out MATCHES "\"id\":2")
  message(FATAL_ERROR "missing shutdown response id in output: ${out}")
endif()
if(NOT out MATCHES "\"result\":null")
  message(FATAL_ERROR "missing shutdown null result in output: ${out}")
endif()
