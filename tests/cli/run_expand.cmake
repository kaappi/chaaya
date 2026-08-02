execute_process(
  COMMAND ${CHAAYA} expand ${INPUT}
  RESULT_VARIABLE rc
  OUTPUT_VARIABLE out
  ERROR_VARIABLE err
)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "expand failed (${rc}): ${err}")
endif()
# `if` in the when template is hygienically renamed (__hyg_N_if) so use-site
# locals cannot capture the keyword (#788); basename still denotes `if`.
if(NOT out MATCHES "\\(__hyg_[0-9]+_if #t \\(begin 1 2\\) #f\\)" AND
   NOT out MATCHES "\\(if #t \\(begin 1 2\\) #f\\)")
  message(FATAL_ERROR "unexpected expand output: ${out}")
endif()
