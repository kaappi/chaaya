cmake_minimum_required(VERSION 3.21)
project(chaaya VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

option(CHAAYA_WARNINGS_AS_ERRORS "Treat warnings as errors" ON)
option(CHAAYA_USE_LINENOISE "Use linenoise for interactive REPL (POSIX)" ON)
option(CHAAYA_WASM "Enable wasm32-wasi target (or stub target)" OFF)

set(CHAAYA_HAS_LINENOISE_VALUE 0)
if(CHAAYA_USE_LINENOISE AND NOT WIN32)
  set(CHAAYA_HAS_LINENOISE_VALUE 1)
endif()

add_library(chaaya_core STATIC
  src/value.c
  src/gc.c
  src/hashtable.c
  src/bignum.c
  src/rational.c
  src/complex.c
  src/reader.c
  src/printer.c
  src/ir.c
  src/ir_lower.c
  src/ir_analyze.c
  src/ir_opt.c
  src/ir_emit.c
  src/compiler.c
  src/expander.c
  src/environment.c
  src/library.c
  src/features.c
  src/fiber.c
  src/reactor.c
  src/ffi.c
  src/llvm_backend.c
  src/lsp.c
  src/vm.c
  src/prim_core.c
  src/prim_control.c
  src/prim_fiber.c
  src/prim_ffi.c
  src/prim_list.c
  src/prim_data.c
  src/prim_bytevector.c
  src/prim_math.c
  src/prim_port.c
  src/prim_error.c
  src/prim_record.c
  src/prim_lazy.c
  src/prim_eval.c
  src/prim_process.c
  src/prim_hashtable.c
  src/prim_random.c
  src/prim_filesystem.c
  src/prim_weak.c
  src/unicode.c
  src/unicode_tables.c
  src/eval.c
  src/cache.c
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

if(UNIX AND NOT APPLE)
  target_link_libraries(chaaya_core PUBLIC dl)
endif()

add_executable(chaaya src/main.c)
target_link_libraries(chaaya PRIVATE chaaya_core)
target_compile_options(chaaya PRIVATE
  -Wall -Wextra -Wpedantic
  $<$<BOOL:${CHAAYA_WARNINGS_AS_ERRORS}>:-Werror>
)

if(CHAAYA_WASM)
  if(CMAKE_SYSTEM_NAME STREQUAL "WASI")
    add_executable(chaaya_wasm src/main.c)
    target_link_libraries(chaaya_wasm PRIVATE chaaya_core)
    target_compile_options(chaaya_wasm PRIVATE
      -Wall -Wextra -Wpedantic
      $<$<BOOL:${CHAAYA_WARNINGS_AS_ERRORS}>:-Werror>
    )
    set_target_properties(chaaya_wasm PROPERTIES OUTPUT_NAME chaaya-wasm)
  else()
    add_executable(chaaya_wasm src/wasm_stub.c)
    target_compile_options(chaaya_wasm PRIVATE
      -Wall -Wextra -Wpedantic
      $<$<BOOL:${CHAAYA_WARNINGS_AS_ERRORS}>:-Werror>
    )
    set_target_properties(chaaya_wasm PROPERTIES OUTPUT_NAME chaaya-wasm)
  endif()
endif()

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
chaaya_add_test(test_ir tests/c/test_ir.c)
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
chaaya_bootstrap_test(base-audit)
chaaya_bootstrap_test(fibers)
chaaya_bootstrap_test(ffi)

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
chaaya_bootstrap_test_libpath(srfi-smoke)
chaaya_bootstrap_test(include-cond)
chaaya_bootstrap_test(records)
chaaya_bootstrap_test(case-lambda)
chaaya_bootstrap_test(lazy)
chaaya_bootstrap_test(multivalue)
chaaya_bootstrap_test(file-ports)
chaaya_bootstrap_test(bytevectors)
chaaya_bootstrap_test(eval)
chaaya_bootstrap_test(exceptions)
chaaya_bootstrap_test(hashtables)
chaaya_bootstrap_test(numeric-io)

# Phase 7A: wire the vendored R7RS suite into CTest. The wrapper records
# section pass/fail counts even when execution is still partial.
add_test(
  NAME r7rs_suite
  COMMAND ${CMAKE_COMMAND}
    -DCHAAYA=$<TARGET_FILE:chaaya>
    -DINPUT=${CMAKE_SOURCE_DIR}/tests/scheme/r7rs/r7rs-tests.scm
    -DLIB_PATH=${CMAKE_SOURCE_DIR}/lib
    -P ${CMAKE_SOURCE_DIR}/tests/scheme/run_r7rs.cmake
)
set_tests_properties(r7rs_suite PROPERTIES TIMEOUT 300)

function(chaaya_kaappi_deferred_test name relpath)
  add_test(
    NAME kaappi_deferred_${name}
    COMMAND ${CMAKE_COMMAND}
      -DCHAAYA=$<TARGET_FILE:chaaya>
      -DINPUT=${CMAKE_SOURCE_DIR}/${relpath}
      -DLIB_PATH=${CMAKE_SOURCE_DIR}/lib
      -P ${CMAKE_SOURCE_DIR}/tests/scheme/run_kaappi_deferred.cmake
  )
  set_tests_properties(kaappi_deferred_${name} PROPERTIES TIMEOUT 120)
endfunction()

# Tier 1 kaappi-deferred compliance (narrow surface, SRFI-64 + scheme base)
chaaya_kaappi_deferred_test(sqrt_exact tests/scheme/kaappi-deferred/compliance/sqrt-exact.scm)
chaaya_kaappi_deferred_test(lists tests/scheme/kaappi-deferred/compliance/lists.scm)
chaaya_kaappi_deferred_test(vectors tests/scheme/kaappi-deferred/compliance/vectors.scm)

# Tier 2 kaappi-deferred compliance (enable individually once local green)
chaaya_kaappi_deferred_test(macro_export_scope tests/scheme/kaappi-deferred/compliance/macro-export-scope.scm)
chaaya_kaappi_deferred_test(chars tests/scheme/kaappi-deferred/compliance/chars.scm)
chaaya_kaappi_deferred_test(bytevectors tests/scheme/kaappi-deferred/compliance/bytevectors.scm)
chaaya_kaappi_deferred_test(eval tests/scheme/kaappi-deferred/compliance/eval.scm)
chaaya_kaappi_deferred_test(lazy tests/scheme/kaappi-deferred/compliance/lazy.scm)

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

add_test(NAME cli_expand
  COMMAND ${CMAKE_COMMAND}
    -DCHAAYA=$<TARGET_FILE:chaaya>
    -DINPUT=${CMAKE_SOURCE_DIR}/tests/cli/expand_input.scm
    -P ${CMAKE_SOURCE_DIR}/tests/cli/run_expand.cmake
)

add_test(NAME cli_check
  COMMAND ${CMAKE_COMMAND}
    -DCHAAYA=$<TARGET_FILE:chaaya>
    -DINPUT=${CMAKE_SOURCE_DIR}/tests/cli/check_ok.scm
    -P ${CMAKE_SOURCE_DIR}/tests/cli/run_check_ok.cmake
)

add_test(NAME cli_check_compile_error
  COMMAND ${CMAKE_COMMAND}
    -DCHAAYA=$<TARGET_FILE:chaaya>
    -DINPUT=${CMAKE_SOURCE_DIR}/tests/cli/check_compile_error.scm
    -P ${CMAKE_SOURCE_DIR}/tests/cli/expect_check_compile_error.cmake
)

add_test(NAME cli_fmt_check
  COMMAND ${CMAKE_COMMAND}
    -DCHAAYA=$<TARGET_FILE:chaaya>
    -DINPUT=${CMAKE_SOURCE_DIR}/tests/cli/fmt_ok.scm
    -P ${CMAKE_SOURCE_DIR}/tests/cli/run_fmt_check.cmake
)

add_test(NAME cli_nyi_compile
  COMMAND ${CMAKE_COMMAND}
    -DCHAAYA=$<TARGET_FILE:chaaya>
    -P ${CMAKE_SOURCE_DIR}/tests/cli/expect_nyi_compile.cmake
)

add_test(NAME cli_nyi_native
  COMMAND ${CMAKE_COMMAND}
    -DCHAAYA=$<TARGET_FILE:chaaya>
    -DINPUT=${CMAKE_SOURCE_DIR}/tests/cli/check_ok.scm
    -P ${CMAKE_SOURCE_DIR}/tests/cli/expect_nyi_native.cmake
)

add_test(NAME cli_nyi_wasm
  COMMAND ${CMAKE_COMMAND}
    -DCHAAYA=$<TARGET_FILE:chaaya>
    -P ${CMAKE_SOURCE_DIR}/tests/cli/expect_nyi_wasm.cmake
)

add_test(NAME cli_lsp_initialize_shutdown
  COMMAND ${CMAKE_COMMAND}
    -DCHAAYA=$<TARGET_FILE:chaaya>
    -P ${CMAKE_SOURCE_DIR}/tests/cli/run_lsp_init_shutdown.cmake
)

add_test(NAME cli_stdin
  COMMAND ${CMAKE_COMMAND}
    -DCHAAYA=$<TARGET_FILE:chaaya>
    -P ${CMAKE_SOURCE_DIR}/tests/cli/run_stdin.cmake
)

install(TARGETS chaaya RUNTIME DESTINATION bin)
