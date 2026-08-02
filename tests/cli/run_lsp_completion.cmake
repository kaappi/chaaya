# completion should include builtins and local definitions.
set(uri "file:///tmp/chaaya-lsp-completion.scm")
set(text "(define local-foo 1)\n(define-syntax local-macro (syntax-rules () ((_ x) x)))\n(local-foo)\n")

# Escape for JSON string.
string(REPLACE "\\" "\\\\" text_json "${text}")
string(REPLACE "\"" "\\\"" text_json "${text_json}")
string(REPLACE "\n" "\\n" text_json "${text_json}")

set(initialize "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{}}")
set(initialized "{\"jsonrpc\":\"2.0\",\"method\":\"initialized\",\"params\":{}}")
set(did_open "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didOpen\",\"params\":{\"textDocument\":{\"uri\":\"${uri}\",\"languageId\":\"scheme\",\"version\":1,\"text\":\"${text_json}\"}}}")
set(completion "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"textDocument/completion\",\"params\":{\"textDocument\":{\"uri\":\"${uri}\"},\"position\":{\"line\":2,\"character\":2}}}")
set(shutdown "{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"shutdown\",\"params\":null}")

string(LENGTH "${initialize}" initialize_len)
string(LENGTH "${initialized}" initialized_len)
string(LENGTH "${did_open}" did_open_len)
string(LENGTH "${completion}" completion_len)
string(LENGTH "${shutdown}" shutdown_len)

set(input_file "${CMAKE_CURRENT_BINARY_DIR}/lsp_completion.input")
file(WRITE "${input_file}"
  "Content-Length: ${initialize_len}\r\n\r\n${initialize}Content-Length: ${initialized_len}\r\n\r\n${initialized}Content-Length: ${did_open_len}\r\n\r\n${did_open}Content-Length: ${completion_len}\r\n\r\n${completion}Content-Length: ${shutdown_len}\r\n\r\n${shutdown}"
)

execute_process(
  COMMAND ${CHAAYA} lsp
  INPUT_FILE "${input_file}"
  RESULT_VARIABLE rc
  OUTPUT_VARIABLE out
  ERROR_VARIABLE err
)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "lsp completion session failed (${rc}): ${err}\n${out}")
endif()
if(NOT out MATCHES "\"id\":3")
  message(FATAL_ERROR "missing completion response id in output: ${out}")
endif()
if(NOT out MATCHES "\"items\"")
  message(FATAL_ERROR "completion response missing items: ${out}")
endif()
if(NOT out MATCHES "\"label\":\"define\"")
  message(FATAL_ERROR "completion response missing builtin label: ${out}")
endif()
if(NOT out MATCHES "\"label\":\"local-foo\"")
  message(FATAL_ERROR "completion response missing local definition label: ${out}")
endif()
