cmake_minimum_required(VERSION 3.20)
project(chaaya VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

option(CHAAYA_WARNINGS_AS_ERRORS "Treat warnings as errors" ON)
option(CHAAYA_USE_LINENOISE "Use linenoise for interactive REPL (POSIX)" ON)

set(CHAAYA_HAS_LINENOISE_VALUE 0)
if(CHAAYA_USE_LINENOISE AND NOT WIN32)
  set(CHAAYA_HAS_LINENOISE_VALUE 1)
endif()

add_library(chaaya_core STATIC
  src/value.c
  src/gc.c
  src/bignum.c
  src/rational.c
  src/complex.c
  src/reader.c
  src/printer.c
  src/compiler.c
  src/expander.c
  src/library.c
  src/features.c
  src/vm.c
  src/prim_core.c
  src/prim_control.c
  src/prim_list.c
  src/prim_data.c
  src/prim_math.c
  src/prim_port.c
  src/prim_record.c
  src/prim_lazy.c
  src/eval.c
  src/cli.c
  src/repl.c
)

target_include_directories(chaaya_core
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_options(chaaya_core PRIVATE
  -Wall -Wextra -Wpedantic
  $<$<BOOL:${CHAAYA_WARNINGS_AS_ERRORS}>:-Werror>
)

if(CHAAYA_HAS_LINENOISE_VALUE)
  add_library(linenoise STATIC third_party/linenoise/linenoise.c)
  target_include_directories(linenoise PUBLIC third_party/linenoise)
  target_compile_options(linenoise PRIVATE -Wall -Wno-unused-parameter)
  target_link_libraries(chaaya_core PUBLIC linenoise)
  target_compile_definitions(chaaya_core PUBLIC CHAAYA_HAS_LINENOISE=1)
endif()

add_executable(chaaya src/main.c)
target_link_libraries(chaaya PRIVATE chaaya_core)
target_compile_options(chaaya PRIVATE
  -Wall -Wextra -Wpedantic
  $<$<BOOL:${CHAAYA_WARNINGS_AS_ERRORS}>:-Werror>
)

enable_testing()

function(chaaya_add_test name)
  add_executable(${name} ${ARGN})
  target_link_libraries(${name} PRIVATE chaaya_core)
  target_compile_options(${name} PRIVATE
    -Wall -Wextra -Wpedantic
    $<$<BOOL:${CHAAYA_WARNINGS_AS_ERRORS}>:-Werror>
  )
  add_test(NAME ${name} COMMAND ${name})
endfunction()

chaaya_add_test(test_smoke tests/c/test_smoke.c)
chaaya_add_test(test_value_gc tests/c/test_value_gc.c)
chaaya_add_test(test_reader_printer tests/c/test_reader_printer.c)
chaaya_add_test(test_vm tests/c/test_vm.c)
chaaya_add_test(test_primitives tests/c/test_primitives.c)
chaaya_add_test(test_cli tests/c/test_cli.c)
chaaya_add_test(test_repl_util tests/c/test_repl_util.c)

function(chaaya_scheme_test name)
  add_test(
    NAME scheme_${name}
    COMMAND ${CMAKE_COMMAND}
      -DCHAAYA=$<TARGET_FILE:chaaya>
      -DINPUT=${CMAKE_SOURCE_DIR}/tests/scheme/${name}.scm
      -DEXPECTED=${CMAKE_SOURCE_DIR}/tests/scheme/${name}.expected
      -P ${CMAKE_SOURCE_DIR}/tests/scheme/run_expect.cmake
  )
endfunction()

chaaya_scheme_test(fact)
chaaya_scheme_test(lists)
chaaya_scheme_test(closures)
chaaya_scheme_test(tail)

# Bootstrap conformance suites (Kaappi-compatible check-* harness, no import)
function(chaaya_bootstrap_test name)
  add_test(
    NAME bootstrap_${name}
    COMMAND ${CMAKE_COMMAND}
      -DCHAAYA=$<TARGET_FILE:chaaya>
      -DHARNESS=${CMAKE_SOURCE_DIR}/tests/scheme/bootstrap/harness.scm
      -DINPUT=${CMAKE_SOURCE_DIR}/tests/scheme/bootstrap/${name}.scm
      -P ${CMAKE_SOURCE_DIR}/tests/scheme/run_bootstrap.cmake
  )
endfunction()

chaaya_bootstrap_test(basic)
chaaya_bootstrap_test(lists)
chaaya_bootstrap_test(control)
chaaya_bootstrap_test(closures)
chaaya_bootstrap_test(equality)
chaaya_bootstrap_test(quote)
chaaya_bootstrap_test(numbers)
chaaya_bootstrap_test(control-cont)
chaaya_bootstrap_test(wave2)
chaaya_bootstrap_test(macros)
chaaya_bootstrap_test(derived)

function(chaaya_bootstrap_test_libpath name)
  add_test(
    NAME bootstrap_${name}
    COMMAND ${CMAKE_COMMAND}
      -DCHAAYA=$<TARGET_FILE:chaaya>
      -DHARNESS=${CMAKE_SOURCE_DIR}/tests/scheme/bootstrap/harness.scm
      -DINPUT=${CMAKE_SOURCE_DIR}/tests/scheme/bootstrap/${name}.scm
      -DLIB_PATH=${CMAKE_SOURCE_DIR}/lib
      -P ${CMAKE_SOURCE_DIR}/tests/scheme/run_bootstrap.cmake
  )
endfunction()

chaaya_bootstrap_test_libpath(libraries)
chaaya_bootstrap_test(include-cond)
chaaya_bootstrap_test(records)
chaaya_bootstrap_test(case-lambda)
chaaya_bootstrap_test(lazy)
chaaya_bootstrap_test(file-ports)

# Display probes adapted from Kaappi differential/probes
function(chaaya_probe_test name)
  add_test(
    NAME probe_${name}
    COMMAND ${CMAKE_COMMAND}
      -DCHAAYA=$<TARGET_FILE:chaaya>
      -DINPUT=${CMAKE_SOURCE_DIR}/tests/scheme/probes/${name}.scm
      -DEXPECTED=${CMAKE_SOURCE_DIR}/tests/scheme/probes/${name}.expected
      -P ${CMAKE_SOURCE_DIR}/tests/scheme/run_expect.cmake
  )
endfunction()

chaaya_probe_test(begin-simplify)
chaaya_probe_test(dead-branch)
chaaya_probe_test(const-fold)
chaaya_probe_test(boolean-simplify)
chaaya_probe_test(identity-elim)
chaaya_probe_test(redefined-builtins)

# CLI integration tests via the chaaya binary
add_test(NAME cli_version COMMAND $<TARGET_FILE:chaaya> --version)
set_tests_properties(cli_version PROPERTIES PASS_REGULAR_EXPRESSION "Chaaya Scheme v0.1.0")

add_test(NAME cli_features COMMAND $<TARGET_FILE:chaaya> features)
set_tests_properties(cli_features PROPERTIES PASS_REGULAR_EXPRESSION "bootstrap")

add_test(NAME cli_ast COMMAND $<TARGET_FILE:chaaya> ast ${CMAKE_SOURCE_DIR}/tests/scheme/fact.scm)
set_tests_properties(cli_ast PROPERTIES PASS_REGULAR_EXPRESSION "define")

add_test(NAME cli_nyi_compile
  COMMAND ${CMAKE_COMMAND}
    -DCHAAYA=$<TARGET_FILE:chaaya>
    -P ${CMAKE_SOURCE_DIR}/tests/cli/expect_nyi_compile.cmake
)

add_test(NAME cli_stdin
  COMMAND ${CMAKE_COMMAND}
    -DCHAAYA=$<TARGET_FILE:chaaya>
    -P ${CMAKE_SOURCE_DIR}/tests/cli/run_stdin.cmake
)

install(TARGETS chaaya RUNTIME DESTINATION bin)
