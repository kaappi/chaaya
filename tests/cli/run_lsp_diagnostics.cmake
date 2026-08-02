# didOpen should publishDiagnostics (possibly empty) for a Scheme buffer.
set(uri "file:///tmp/chaaya-lsp-diag.scm")
set(text "(define x )")
# Escape for JSON string
string(REPLACE "\\" "\\\\" text_json "${text}")
string(REPLACE "\"" "\\\"" text_json "${text_json}")

set(initialize "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{}}")
set(initialized "{\"jsonrpc\":\"2.0\",\"method\":\"initialized\",\"params\":{}}")
set(did_open "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didOpen\",\"params\":{\"textDocument\":{\"uri\":\"${uri}\",\"languageId\":\"scheme\",\"version\":1,\"text\":\"${text_json}\"}}}")
set(shutdown "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"shutdown\",\"params\":null}")

string(LENGTH "${initialize}" initialize_len)
string(LENGTH "${initialized}" initialized_len)
string(LENGTH "${did_open}" did_open_len)
string(LENGTH "${shutdown}" shutdown_len)

set(input_file "${CMAKE_CURRENT_BINARY_DIR}/lsp_diagnostics.input")
file(WRITE "${input_file}"
  "Content-Length: ${initialize_len}\r\n\r\n${initialize}Content-Length: ${initialized_len}\r\n\r\n${initialized}Content-Length: ${did_open_len}\r\n\r\n${did_open}Content-Length: ${shutdown_len}\r\n\r\n${shutdown}"
)

execute_process(
  COMMAND ${CHAAYA} lsp
  INPUT_FILE "${input_file}"
  RESULT_VARIABLE rc
  OUTPUT_VARIABLE out
  ERROR_VARIABLE err
)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "lsp diagnostics session failed (${rc}): ${err}\n${out}")
endif()
if(NOT out MATCHES "publishDiagnostics")
  message(FATAL_ERROR "expected publishDiagnostics notification, got: ${out}")
endif()
if(NOT out MATCHES "\"uri\"")
  message(FATAL_ERROR "expected diagnostics uri field, got: ${out}")
endif()
