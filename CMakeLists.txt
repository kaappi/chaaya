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
  src/reader_number.c
  src/printer.c
  src/ir.c
  src/ir_lower.c
  src/ir_analyze.c
  src/ir_opt.c
  src/ir_emit.c
  src/compiler.c
  src/compiler_bind.c
  src/compiler_control.c
  src/expander_syntax_rules.c
  src/expander_toplevel.c
  src/environment.c
  src/library.c
  src/library_builtin.c
  src/features.c
  src/diagnostics.c
  src/doctor.c
  src/fiber.c
  src/channel.c
  src/reactor.c
  src/thread.c
  src/gc_deep_copy.c
  src/shared_channel.c
  src/ffi.c
  src/ffi_callback.c
  src/llvm_backend.c
  src/lsp.c
  src/lsp_analysis.c
  src/runtime_exports.c
  src/vm.c
  src/prim_core.c
  src/prim_control.c
  src/prim_fiber.c
  src/prim_ffi.c
  src/prim_list.c
  src/prim_data.c
  src/prim_char.c
  src/prim_string.c
  src/prim_vector.c
  src/prim_utf8.c
  src/prim_bytevector.c
  src/prim_math.c
  src/prim_port.c
  src/prim_port_io.c
  src/prim_error.c
  src/prim_record.c
  src/prim_lazy.c
  src/prim_eval.c
  src/prim_process.c
  src/prim_hashtable.c
  src/prim_random.c
  src/prim_filesystem.c
  src/prim_weak.c
  src/prim_srfi1.c
  src/prim_srfi13.c
  src/prim_srfi133.c
  src/prim_srfi258.c
  src/prim_srfi260.c
  src/unicode.c
  src/unicode_tables.c
  src/eval.c
  src/cache.c
  src/disasm.c
  src/timings.c
  src/test_runner.c
  src/fmt.c
  src/cli.c
  src/cli_parse.c
  src/cli_cmds.c
  src/repl.c
  src/sandbox.c
  src/profile.c
  src/coverage.c
)

target_include_directories(chaaya_core
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_compile_definitions(chaaya_core
  PUBLIC
    CHAAYA_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    CHAAYA_BUILD_DIR="${CMAKE_BINARY_DIR}"
)

target_compile_options(chaaya_core PRIVATE
  -Wall -Wextra -Wpedantic
  $<$<BOOL:${CHAAYA_WARNINGS_AS_ERRORS}>:-Werror>
)

# Full runtime archive for LLVM-native binaries (same objects as chaaya_core + exports).
add_library(chaaya_rt STATIC
  src/value.c
  src/gc.c
  src/hashtable.c
  src/bignum.c
  src/rational.c
  src/complex.c
  src/reader.c
  src/reader_number.c
  src/printer.c
  src/ir.c
  src/ir_lower.c
  src/ir_analyze.c
  src/ir_opt.c
  src/ir_emit.c
  src/compiler.c
  src/compiler_bind.c
  src/compiler_control.c
  src/expander_syntax_rules.c
  src/expander_toplevel.c
  src/environment.c
  src/library.c
  src/library_builtin.c
  src/features.c
  src/diagnostics.c
  src/fiber.c
  src/channel.c
  src/reactor.c
  src/thread.c
  src/gc_deep_copy.c
  src/shared_channel.c
  src/ffi.c
  src/ffi_callback.c
  src/vm.c
  src/prim_core.c
  src/prim_control.c
  src/prim_fiber.c
  src/prim_ffi.c
  src/prim_list.c
  src/prim_data.c
  src/prim_char.c
  src/prim_string.c
  src/prim_vector.c
  src/prim_utf8.c
  src/prim_bytevector.c
  src/prim_math.c
  src/prim_port.c
  src/prim_port_io.c
  src/prim_error.c
  src/prim_record.c
  src/prim_lazy.c
  src/prim_eval.c
  src/prim_process.c
  src/prim_hashtable.c
  src/prim_random.c
  src/prim_filesystem.c
  src/prim_weak.c
  src/prim_srfi1.c
  src/prim_srfi13.c
  src/prim_srfi133.c
  src/prim_srfi258.c
  src/prim_srfi260.c
  src/unicode.c
  src/unicode_tables.c
  src/eval.c
  src/cache.c
  src/disasm.c
  src/timings.c
  src/sandbox.c
  src/profile.c
  src/coverage.c
  src/runtime_exports.c
)
target_include_directories(chaaya_rt PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_definitions(chaaya_rt
  PUBLIC
    CHAAYA_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    CHAAYA_BUILD_DIR="${CMAKE_BINARY_DIR}"
)
target_compile_options(chaaya_rt PRIVATE
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
  target_link_libraries(chaaya_rt PUBLIC dl)
endif()

find_package(Threads REQUIRED)
target_link_libraries(chaaya_core PUBLIC Threads::Threads)
target_link_libraries(chaaya_rt PUBLIC Threads::Threads)

add_executable(chaaya src/main.c)
target_link_libraries(chaaya PRIVATE chaaya_core)
add_dependencies(chaaya chaaya_rt)
target_compile_options(chaaya PRIVATE
  -Wall -Wextra -Wpedantic
  $<$<BOOL:${CHAAYA_WARNINGS_AS_ERRORS}>:-Werror>
)

if(CHAAYA_WASM)
  if(CMAKE_SYSTEM_NAME STREQUAL "WASI")
    add_executable(chaaya_wasm src/main.c)
    target_link_libraries(chaaya_wasm PRIVATE chaaya_core)
    target_compile_definitions(chaaya_core PUBLIC CHAAYA_WASM=1)
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
chaaya_add_test(test_fiber_io tests/c/test_fiber_io.c)
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
chaaya_kaappi_deferred_test(numeric_gaps tests/scheme/kaappi-deferred/compliance/numeric-gaps.scm)
chaaya_kaappi_deferred_test(reader_numerics tests/scheme/kaappi-deferred/compliance/reader-numerics.scm)
chaaya_kaappi_deferred_test(reader_surrogates tests/scheme/kaappi-deferred/compliance/reader-surrogates.scm)
chaaya_kaappi_deferred_test(bignum_rational_gc tests/scheme/kaappi-deferred/compliance/bignum-rational-gc.scm)
chaaya_kaappi_deferred_test(reader_delimiter_gaps tests/scheme/kaappi-deferred/compliance/reader-delimiter-gaps.scm)
chaaya_kaappi_deferred_test(fold_case_unicode tests/scheme/kaappi-deferred/compliance/fold-case-unicode.scm)
chaaya_kaappi_deferred_test(r7rs_chars_strings_gaps tests/scheme/kaappi-deferred/compliance/r7rs-chars-strings-gaps.scm)
chaaya_kaappi_deferred_test(unicode tests/scheme/kaappi-deferred/compliance/unicode.scm)
chaaya_kaappi_deferred_test(unicode_case tests/scheme/kaappi-deferred/compliance/unicode-case.scm)
chaaya_kaappi_deferred_test(strings tests/scheme/kaappi-deferred/compliance/strings.scm)
chaaya_kaappi_deferred_test(complex_strings tests/scheme/kaappi-deferred/compliance/complex-strings.scm)
chaaya_kaappi_deferred_test(string_ports tests/scheme/kaappi-deferred/compliance/string-ports.scm)
chaaya_kaappi_deferred_test(r7rs_datatypes_gaps tests/scheme/kaappi-deferred/compliance/r7rs-datatypes-gaps.scm)
chaaya_kaappi_deferred_test(time tests/scheme/kaappi-deferred/compliance/time.scm)
chaaya_kaappi_deferred_test(process_context tests/scheme/kaappi-deferred/compliance/process-context.scm)
chaaya_kaappi_deferred_test(scheme_repl tests/scheme/kaappi-deferred/compliance/scheme-repl.scm)
chaaya_kaappi_deferred_test(include_lib_decls tests/scheme/kaappi-deferred/compliance/include-lib-decls.scm)
chaaya_kaappi_deferred_test(correctness_fixes tests/scheme/kaappi-deferred/compliance/correctness-fixes.scm)
chaaya_kaappi_deferred_test(final_gaps tests/scheme/kaappi-deferred/compliance/final-gaps.scm)
chaaya_kaappi_deferred_test(bugfixes tests/scheme/kaappi-deferred/compliance/bugfixes.scm)
chaaya_kaappi_deferred_test(deferred_bugfixes tests/scheme/kaappi-deferred/compliance/deferred-bugfixes.scm)
chaaya_kaappi_deferred_test(deferred_final tests/scheme/kaappi-deferred/compliance/deferred-final.scm)
chaaya_kaappi_deferred_test(r7rs_thin_forms_gaps tests/scheme/kaappi-deferred/compliance/r7rs-thin-forms-gaps.scm)
chaaya_kaappi_deferred_test(r7rs_tail_position_gaps tests/scheme/kaappi-deferred/compliance/r7rs-tail-position-gaps.scm)
chaaya_kaappi_deferred_test(r7rs_import_macro_gaps tests/scheme/kaappi-deferred/compliance/r7rs-import-macro-gaps.scm)
chaaya_kaappi_deferred_test(r7rs_libraries_gaps tests/scheme/kaappi-deferred/compliance/r7rs-libraries-gaps.scm)
chaaya_kaappi_deferred_test(printer_gaps tests/scheme/kaappi-deferred/compliance/printer-gaps.scm)
chaaya_kaappi_deferred_test(reader_exactness_gaps tests/scheme/kaappi-deferred/compliance/reader-exactness-gaps.scm)
chaaya_kaappi_deferred_test(srfi_completeness tests/scheme/kaappi-deferred/compliance/srfi-completeness.scm)
chaaya_kaappi_deferred_test(r7rs_control_io_gaps tests/scheme/kaappi-deferred/compliance/r7rs-control-io-gaps.scm)
chaaya_kaappi_deferred_test(reader_port_refill_gaps tests/scheme/kaappi-deferred/compliance/reader-port-refill-gaps.scm)
chaaya_kaappi_deferred_test(r7rs_expressions_gaps tests/scheme/kaappi-deferred/compliance/r7rs-expressions-gaps.scm)
chaaya_kaappi_deferred_test(r7rs_hygiene_gaps tests/scheme/kaappi-deferred/compliance/r7rs-hygiene-gaps.scm)
chaaya_kaappi_deferred_test(r7rs_continuation_gaps tests/scheme/kaappi-deferred/compliance/r7rs-continuation-gaps.scm)
chaaya_kaappi_deferred_test(r7rs_tail_procedures_gaps tests/scheme/kaappi-deferred/compliance/r7rs-tail-procedures-gaps.scm)
chaaya_kaappi_deferred_test(record_ctor_clause_keyword_1882 tests/scheme/kaappi-deferred/compliance/record-ctor-clause-keyword-1882.scm)

# Tier 3 kaappi-deferred smoke
chaaya_kaappi_deferred_test(smoke_basic tests/scheme/kaappi-deferred/smoke/basic.scm)
chaaya_kaappi_deferred_test(smoke_numeric tests/scheme/kaappi-deferred/smoke/numeric.scm)
chaaya_kaappi_deferred_test(smoke_macros tests/scheme/kaappi-deferred/smoke/macros.scm)
chaaya_kaappi_deferred_test(smoke_expander_fixes tests/scheme/kaappi-deferred/smoke/expander-fixes.scm)
chaaya_kaappi_deferred_test(smoke_expander_many_patvars tests/scheme/kaappi-deferred/smoke/expander-many-patvars.scm)
chaaya_kaappi_deferred_test(smoke_expander_vector_patterns tests/scheme/kaappi-deferred/smoke/expander-vector-patterns.scm)
chaaya_kaappi_deferred_test(smoke_lambda_param_shadows_keyword_788 tests/scheme/kaappi-deferred/smoke/lambda-param-shadows-keyword-788.scm)
chaaya_kaappi_deferred_test(smoke_internal_define_syntax_scope tests/scheme/kaappi-deferred/smoke/internal-define-syntax-scope.scm)
chaaya_kaappi_deferred_test(smoke_library_macro_leak_877 tests/scheme/kaappi-deferred/smoke/library-macro-leak-877.scm)
chaaya_kaappi_deferred_test(smoke_record_macro_scope_1718 tests/scheme/kaappi-deferred/smoke/record-macro-scope-1718.scm)
chaaya_kaappi_deferred_test(smoke_macro_chains tests/scheme/kaappi-deferred/smoke/macro-chains.scm)
chaaya_kaappi_deferred_test(smoke_gc_mark_contents_types tests/scheme/kaappi-deferred/smoke/gc-mark-contents-types.scm)
chaaya_kaappi_deferred_test(smoke_bignum_rational_arith tests/scheme/kaappi-deferred/smoke/bignum-rational-arith.scm)
chaaya_kaappi_deferred_test(smoke_bignum_division_multi_arg tests/scheme/kaappi-deferred/smoke/bignum-division-multi-arg.scm)
chaaya_kaappi_deferred_test(smoke_bignum_rational_normalization tests/scheme/kaappi-deferred/smoke/bignum-rational-normalization.scm)
chaaya_kaappi_deferred_test(smoke_not_not_non_boolean tests/scheme/kaappi-deferred/smoke/not-not-non-boolean.scm)
chaaya_kaappi_deferred_test(smoke_zero_pred_type_error tests/scheme/kaappi-deferred/smoke/zero-pred-type-error.scm)
chaaya_kaappi_deferred_test(smoke_literal_immutability tests/scheme/kaappi-deferred/smoke/literal-immutability.scm)
chaaya_kaappi_deferred_test(smoke_apply_shadowing tests/scheme/kaappi-deferred/smoke/apply-shadowing.scm)
chaaya_kaappi_deferred_test(smoke_call_with_values_arity tests/scheme/kaappi-deferred/smoke/call-with-values-arity.scm)
chaaya_kaappi_deferred_test(smoke_binding_form_validation tests/scheme/kaappi-deferred/smoke/binding-form-validation.scm)
chaaya_kaappi_deferred_test(smoke_memv_assv_numeric tests/scheme/kaappi-deferred/smoke/memv-assv-numeric.scm)
chaaya_kaappi_deferred_test(smoke_char_folding_fixes tests/scheme/kaappi-deferred/smoke/char-folding-fixes.scm)
chaaya_kaappi_deferred_test(smoke_expt_rational tests/scheme/kaappi-deferred/smoke/expt-rational.scm)
chaaya_kaappi_deferred_test(smoke_tail_calls tests/scheme/kaappi-deferred/smoke/tail-calls.scm)
# Batch 2 smoke (GC, fibers, bignum, expander, ports — verified local green)
chaaya_kaappi_deferred_test(smoke_bignum_expt_gc tests/scheme/kaappi-deferred/smoke/bignum-expt-gc.scm)
chaaya_kaappi_deferred_test(smoke_closure_upvalue_gc tests/scheme/kaappi-deferred/smoke/closure-upvalue-gc.scm)
chaaya_kaappi_deferred_test(smoke_fiber_channel_gc tests/scheme/kaappi-deferred/smoke/fiber-channel-gc.scm)
chaaya_kaappi_deferred_test(smoke_fiber_channel_close tests/scheme/kaappi-deferred/smoke/fiber-channel-close.scm)
chaaya_kaappi_deferred_test(smoke_fiber_gc_remembered_set tests/scheme/kaappi-deferred/smoke/fiber-gc-remembered-set.scm)
chaaya_kaappi_deferred_test(smoke_promise_gc tests/scheme/kaappi-deferred/smoke/promise-gc.scm)
chaaya_kaappi_deferred_test(smoke_mutation_write_barrier tests/scheme/kaappi-deferred/smoke/mutation-write-barrier.scm)
chaaya_kaappi_deferred_test(smoke_upvalue_write_barrier tests/scheme/kaappi-deferred/smoke/upvalue-write-barrier.scm)
chaaya_kaappi_deferred_test(smoke_ellipsis_mismatch tests/scheme/kaappi-deferred/smoke/ellipsis-mismatch.scm)
chaaya_kaappi_deferred_test(smoke_import_composed tests/scheme/kaappi-deferred/smoke/import-composed.scm)
chaaya_kaappi_deferred_test(smoke_portable_srfi_import tests/scheme/kaappi-deferred/smoke/portable-srfi-import.scm)
chaaya_kaappi_deferred_test(smoke_bytevector_port_fixes tests/scheme/kaappi-deferred/smoke/bytevector-port-fixes.scm)
chaaya_kaappi_deferred_test(smoke_guard_continuable_845 tests/scheme/kaappi-deferred/smoke/guard-continuable-845.scm)
chaaya_kaappi_deferred_test(smoke_arith_overflow tests/scheme/kaappi-deferred/smoke/arith-overflow.scm)
chaaya_kaappi_deferred_test(smoke_bare_lambda_self_tail_call tests/scheme/kaappi-deferred/smoke/bare-lambda-self-tail-call.scm)
chaaya_kaappi_deferred_test(smoke_bignum_gc_alias_1414 tests/scheme/kaappi-deferred/smoke/bignum-gc-alias-1414.scm)
chaaya_kaappi_deferred_test(smoke_callwithargs_bounds tests/scheme/kaappi-deferred/smoke/callwithargs-bounds.scm)
chaaya_kaappi_deferred_test(smoke_case_arrow_register tests/scheme/kaappi-deferred/smoke/case-arrow-register.scm)
chaaya_kaappi_deferred_test(smoke_continuation_wind_870 tests/scheme/kaappi-deferred/smoke/continuation-wind-870.scm)
chaaya_kaappi_deferred_test(smoke_define_values_gc tests/scheme/kaappi-deferred/smoke/define-values-gc.scm)
chaaya_kaappi_deferred_test(smoke_dynamic_wind_double_875 tests/scheme/kaappi-deferred/smoke/dynamic-wind-double-875.scm)
chaaya_kaappi_deferred_test(smoke_exact_inexact_842_848 tests/scheme/kaappi-deferred/smoke/exact-inexact-842-848.scm)
chaaya_kaappi_deferred_test(smoke_fiber_round_robin tests/scheme/kaappi-deferred/smoke/fiber-round-robin.scm)
chaaya_kaappi_deferred_test(smoke_fiber_sleep_does_not_stall_sibling tests/scheme/kaappi-deferred/smoke/fiber-sleep-does-not-stall-sibling.scm)
chaaya_kaappi_deferred_test(smoke_thread_sleep_876 tests/scheme/kaappi-deferred/smoke/thread-sleep-876.scm)
chaaya_kaappi_deferred_test(smoke_fiber_channel_timeout tests/scheme/kaappi-deferred/smoke/fiber-channel-timeout.scm)
chaaya_kaappi_deferred_test(smoke_thread_join_timeout_878 tests/scheme/kaappi-deferred/smoke/thread-join-timeout-878.scm)
chaaya_kaappi_deferred_test(smoke_kaappi_parallel_pool tests/scheme/kaappi-deferred/smoke/kaappi-parallel-pool.scm)
chaaya_kaappi_deferred_test(smoke_kaappi_parallel_shutdown tests/scheme/kaappi-deferred/smoke/kaappi-parallel-shutdown.scm)
chaaya_kaappi_deferred_test(smoke_fiber_channel_rendezvous_thread_lite tests/scheme/kaappi-deferred/smoke/fiber-channel-rendezvous-thread-lite.scm)
chaaya_kaappi_deferred_test(smoke_fiber_channel_rendezvous_thread tests/scheme/kaappi-deferred/smoke/fiber-channel-rendezvous-thread.scm)
chaaya_kaappi_deferred_test(smoke_fiber_channel_lost_wakeup_1489 tests/scheme/kaappi-deferred/smoke/fiber-channel-lost-wakeup-1489.scm)
chaaya_kaappi_deferred_test(smoke_hash_table_walk_rehash tests/scheme/kaappi-deferred/smoke/hash-table-walk-rehash.scm)
chaaya_kaappi_deferred_test(smoke_inexact_to_exact tests/scheme/kaappi-deferred/smoke/inexact-to-exact.scm)
chaaya_kaappi_deferred_test(smoke_rational_rounding tests/scheme/kaappi-deferred/smoke/rational-rounding.scm)
chaaya_kaappi_deferred_test(smoke_string_trim_whitespace_826 tests/scheme/kaappi-deferred/smoke/string-trim-whitespace-826.scm)
# Batch 3 smoke (triage-green)
chaaya_kaappi_deferred_test(smoke_bignum_toF64_833 tests/scheme/kaappi-deferred/smoke/bignum-toF64-833.scm)
chaaya_kaappi_deferred_test(smoke_call_global_continuation tests/scheme/kaappi-deferred/smoke/call-global-continuation.scm)
chaaya_kaappi_deferred_test(smoke_case_empty_datum_854 tests/scheme/kaappi-deferred/smoke/case-empty-datum-854.scm)
chaaya_kaappi_deferred_test(smoke_command_line_o_flag tests/scheme/kaappi-deferred/smoke/command-line-o-flag.scm)
chaaya_kaappi_deferred_test(smoke_complex_accessors_types tests/scheme/kaappi-deferred/smoke/complex-accessors-types.scm)
chaaya_kaappi_deferred_test(smoke_cond_expand_empty tests/scheme/kaappi-deferred/smoke/cond-expand-empty.scm)
chaaya_kaappi_deferred_test(smoke_constant_fold_shadowing tests/scheme/kaappi-deferred/smoke/constant-fold-shadowing.scm)
chaaya_kaappi_deferred_test(smoke_continuation_gc_size tests/scheme/kaappi-deferred/smoke/continuation-gc-size.scm)
chaaya_kaappi_deferred_test(smoke_create_temp_file_error tests/scheme/kaappi-deferred/smoke/create-temp-file-error.scm)
chaaya_kaappi_deferred_test(smoke_datum_label_vector tests/scheme/kaappi-deferred/smoke/datum-label-vector.scm)
chaaya_kaappi_deferred_test(smoke_deep_mark_864 tests/scheme/kaappi-deferred/smoke/deep-mark-864.scm)
chaaya_kaappi_deferred_test(smoke_define_values_lambda_body tests/scheme/kaappi-deferred/smoke/define-values-lambda-body.scm)
chaaya_kaappi_deferred_test(smoke_derived tests/scheme/kaappi-deferred/smoke/derived.scm)
chaaya_kaappi_deferred_test(smoke_do_closure_capture_803 tests/scheme/kaappi-deferred/smoke/do-closure-capture-803.scm)
chaaya_kaappi_deferred_test(smoke_dotted_pair_datum_comment tests/scheme/kaappi-deferred/smoke/dotted-pair-datum-comment.scm)
chaaya_kaappi_deferred_test(smoke_ellipsis_depth_mismatch tests/scheme/kaappi-deferred/smoke/ellipsis-depth-mismatch.scm)
chaaya_kaappi_deferred_test(smoke_exact_integer_sqrt_export tests/scheme/kaappi-deferred/smoke/exact-integer-sqrt-export.scm)
chaaya_kaappi_deferred_test(smoke_exact_large_float tests/scheme/kaappi-deferred/smoke/exact-large-float.scm)
chaaya_kaappi_deferred_test(smoke_exactness_prefix tests/scheme/kaappi-deferred/smoke/exactness-prefix.scm)
chaaya_kaappi_deferred_test(smoke_case_lambda_fixes tests/scheme/kaappi-deferred/smoke/case-lambda-fixes.scm)
chaaya_kaappi_deferred_test(smoke_expt_negative_base_1725 tests/scheme/kaappi-deferred/smoke/expt-negative-base-1725.scm)
chaaya_kaappi_deferred_test(smoke_equal_dag tests/scheme/kaappi-deferred/smoke/equal-dag.scm)
chaaya_kaappi_deferred_test(smoke_circular_list_terminate tests/scheme/kaappi-deferred/smoke/circular-list-terminate.scm)
chaaya_kaappi_deferred_test(smoke_env_uaf tests/scheme/kaappi-deferred/smoke/env-uaf.scm)
chaaya_kaappi_deferred_test(smoke_export_rename tests/scheme/kaappi-deferred/smoke/export-rename.scm)
chaaya_kaappi_deferred_test(smoke_features_consistency tests/scheme/kaappi-deferred/smoke/features-consistency.scm)
# Batch 4 smoke (triage-green after exact-complex)
chaaya_kaappi_deferred_test(smoke_deep_nesting_print tests/scheme/kaappi-deferred/smoke/deep-nesting-print.scm)
chaaya_kaappi_deferred_test(smoke_deep_recursion tests/scheme/kaappi-deferred/smoke/deep-recursion.scm)
chaaya_kaappi_deferred_test(smoke_ffi_nul_string tests/scheme/kaappi-deferred/smoke/ffi-nul-string.scm)
chaaya_kaappi_deferred_test(smoke_fiber_channel_capacity tests/scheme/kaappi-deferred/smoke/fiber-channel-capacity.scm)
chaaya_kaappi_deferred_test(smoke_fiber_concurrent_many tests/scheme/kaappi-deferred/smoke/fiber-concurrent-many.scm)
chaaya_kaappi_deferred_test(smoke_fiber_yield_reentrant tests/scheme/kaappi-deferred/smoke/fiber-yield-reentrant.scm)
chaaya_kaappi_deferred_test(smoke_macro_import_leak tests/scheme/kaappi-deferred/smoke/macro-import-leak.scm)
chaaya_kaappi_deferred_test(smoke_record_cycle_deepcopy tests/scheme/kaappi-deferred/smoke/record-cycle-deepcopy.scm)
chaaya_kaappi_deferred_test(smoke_top_level_yield tests/scheme/kaappi-deferred/smoke/top-level-yield.scm)
# Batch 5 smoke (SRFI-13 / cxr / complex-neg-zero)
chaaya_kaappi_deferred_test(smoke_string_pad_multibyte tests/scheme/kaappi-deferred/smoke/string-pad-multibyte.scm)
chaaya_kaappi_deferred_test(smoke_string_replace_range tests/scheme/kaappi-deferred/smoke/string-replace-range.scm)
chaaya_kaappi_deferred_test(smoke_string_titlecase tests/scheme/kaappi-deferred/smoke/string-titlecase.scm)
chaaya_kaappi_deferred_test(smoke_string_join_grammar tests/scheme/kaappi-deferred/smoke/string-join-grammar.scm)
chaaya_kaappi_deferred_test(smoke_string_ext_bounds tests/scheme/kaappi-deferred/smoke/string-ext-bounds.scm)
chaaya_kaappi_deferred_test(smoke_srfi13_start2_end2 tests/scheme/kaappi-deferred/smoke/srfi13-start2-end2.scm)
chaaya_kaappi_deferred_test(smoke_libraries tests/scheme/kaappi-deferred/smoke/libraries.scm)
chaaya_kaappi_deferred_test(smoke_datum_label_gc tests/scheme/kaappi-deferred/smoke/datum-label-gc.scm)
chaaya_kaappi_deferred_test(smoke_deep_copy_string_bv_807 tests/scheme/kaappi-deferred/smoke/deep-copy-string-bv-807.scm)
chaaya_kaappi_deferred_test(smoke_gc_rooting_stress tests/scheme/kaappi-deferred/smoke/gc-rooting-stress.scm)
chaaya_kaappi_deferred_test(smoke_complex_neg_zero tests/scheme/kaappi-deferred/smoke/complex-neg-zero.scm)
# Batch 6 smoke
chaaya_kaappi_deferred_test(smoke_char_unicode_properties tests/scheme/kaappi-deferred/smoke/char-unicode-properties.scm)
chaaya_kaappi_deferred_test(smoke_gc_numeric_rooting tests/scheme/kaappi-deferred/smoke/gc-numeric-rooting.scm)
chaaya_kaappi_deferred_test(smoke_map_gc_root_1093 tests/scheme/kaappi-deferred/smoke/map-gc-root-1093.scm)
chaaya_kaappi_deferred_test(smoke_fiber_native_thunk tests/scheme/kaappi-deferred/smoke/fiber-native-thunk.scm)
chaaya_kaappi_deferred_test(smoke_fiber_slot_reuse tests/scheme/kaappi-deferred/smoke/fiber-slot-reuse.scm)
chaaya_kaappi_deferred_test(smoke_gcd_gc_843 tests/scheme/kaappi-deferred/smoke/gcd-gc-843.scm)
chaaya_kaappi_deferred_test(smoke_global_redefine_fold tests/scheme/kaappi-deferred/smoke/global-redefine-fold.scm)
chaaya_kaappi_deferred_test(smoke_import_modifier_macros tests/scheme/kaappi-deferred/smoke/import-modifier-macros.scm)
chaaya_kaappi_deferred_test(smoke_inexact_zero_division tests/scheme/kaappi-deferred/smoke/inexact-zero-division.scm)
chaaya_kaappi_deferred_test(smoke_internal_define tests/scheme/kaappi-deferred/smoke/internal-define.scm)
chaaya_kaappi_deferred_test(smoke_lambda_leak_832 tests/scheme/kaappi-deferred/smoke/lambda-leak-832.scm)
chaaya_kaappi_deferred_test(smoke_lambda_param_shadow_fold_790 tests/scheme/kaappi-deferred/smoke/lambda-param-shadow-fold-790.scm)
chaaya_kaappi_deferred_test(smoke_let_syntax_many_bindings tests/scheme/kaappi-deferred/smoke/let-syntax-many-bindings.scm)
chaaya_kaappi_deferred_test(smoke_let_values_scoping tests/scheme/kaappi-deferred/smoke/let-values-scoping.scm)
chaaya_kaappi_deferred_test(smoke_letrec_local_bindings tests/scheme/kaappi-deferred/smoke/letrec-local-bindings.scm)
chaaya_kaappi_deferred_test(smoke_library_macro_internals tests/scheme/kaappi-deferred/smoke/library-macro-internals.scm)
chaaya_kaappi_deferred_test(smoke_library_redefine tests/scheme/kaappi-deferred/smoke/library-redefine.scm)
chaaya_kaappi_deferred_test(smoke_load_env_1190 tests/scheme/kaappi-deferred/smoke/load-env-1190.scm)
chaaya_kaappi_deferred_test(smoke_magnitude_runtime_fixes tests/scheme/kaappi-deferred/smoke/magnitude-runtime-fixes.scm)
chaaya_kaappi_deferred_test(smoke_misc_830_835 tests/scheme/kaappi-deferred/smoke/misc-830-835.scm)
chaaya_kaappi_deferred_test(smoke_mul_zero_side_effects tests/scheme/kaappi-deferred/smoke/mul-zero-side-effects.scm)
chaaya_kaappi_deferred_test(smoke_native_arith_regression tests/scheme/kaappi-deferred/smoke/native-arith-regression.scm)
chaaya_kaappi_deferred_test(smoke_native_self_call_regression tests/scheme/kaappi-deferred/smoke/native-self-call-regression.scm)
chaaya_kaappi_deferred_test(smoke_nested_ellipsis_depth2 tests/scheme/kaappi-deferred/smoke/nested-ellipsis-depth2.scm)
chaaya_kaappi_deferred_test(smoke_nested_lib_env_root tests/scheme/kaappi-deferred/smoke/nested-lib-env-root.scm)
chaaya_kaappi_deferred_test(smoke_number_string_bignum_radix tests/scheme/kaappi-deferred/smoke/number-string-bignum-radix.scm)
chaaya_kaappi_deferred_test(smoke_numden_flonum_858 tests/scheme/kaappi-deferred/smoke/numden-flonum-858.scm)
chaaya_kaappi_deferred_test(smoke_numeric_edge_846_853 tests/scheme/kaappi-deferred/smoke/numeric-edge-846-853.scm)
chaaya_kaappi_deferred_test(smoke_numeric_overflow tests/scheme/kaappi-deferred/smoke/numeric-overflow.scm)
chaaya_kaappi_deferred_test(smoke_parameterize_eval_860 tests/scheme/kaappi-deferred/smoke/parameterize-eval-860.scm)
chaaya_kaappi_deferred_test(smoke_passthrough_redefine_constfold tests/scheme/kaappi-deferred/smoke/passthrough-redefine-constfold.scm)
chaaya_kaappi_deferred_test(smoke_peek_char_fd_utf8_798 tests/scheme/kaappi-deferred/smoke/peek-char-fd-utf8-798.scm)
chaaya_kaappi_deferred_test(smoke_peek_char_utf8 tests/scheme/kaappi-deferred/smoke/peek-char-utf8.scm)
chaaya_kaappi_deferred_test(smoke_port_param_gc_1013 tests/scheme/kaappi-deferred/smoke/port-param-gc-1013.scm)
chaaya_kaappi_deferred_test(smoke_port_parameterize_811 tests/scheme/kaappi-deferred/smoke/port-parameterize-811.scm)
chaaya_kaappi_deferred_test(smoke_port_write_buffer tests/scheme/kaappi-deferred/smoke/port-write-buffer.scm)
chaaya_kaappi_deferred_test(smoke_ppvalue_dotted_tail tests/scheme/kaappi-deferred/smoke/ppvalue-dotted-tail.scm)
chaaya_kaappi_deferred_test(smoke_prettyprint_cycle_859 tests/scheme/kaappi-deferred/smoke/prettyprint-cycle-859.scm)
chaaya_kaappi_deferred_test(smoke_prettyprint_width_921 tests/scheme/kaappi-deferred/smoke/prettyprint-width-921.scm)
chaaya_kaappi_deferred_test(smoke_printer_string_fixes tests/scheme/kaappi-deferred/smoke/printer-string-fixes.scm)
chaaya_kaappi_deferred_test(smoke_quasiquote_nested_849_850_852 tests/scheme/kaappi-deferred/smoke/quasiquote-nested-849-850-852.scm)
chaaya_kaappi_deferred_test(smoke_quasiquote_splice_nested tests/scheme/kaappi-deferred/smoke/quasiquote-splice-nested.scm)
chaaya_kaappi_deferred_test(smoke_rational_bignum_838_839 tests/scheme/kaappi-deferred/smoke/rational-bignum-838-839.scm)
chaaya_kaappi_deferred_test(smoke_read_bytevector_large_k tests/scheme/kaappi-deferred/smoke/read-bytevector-large-k.scm)
chaaya_kaappi_deferred_test(smoke_read_bytevector_zero_length tests/scheme/kaappi-deferred/smoke/read-bytevector-zero-length.scm)
chaaya_kaappi_deferred_test(smoke_read_peek_reorder_804 tests/scheme/kaappi-deferred/smoke/read-peek-reorder-804.scm)
chaaya_kaappi_deferred_test(smoke_read_string_port_peek_799 tests/scheme/kaappi-deferred/smoke/read-string-port-peek-799.scm)
chaaya_kaappi_deferred_test(smoke_reader_long_list tests/scheme/kaappi-deferred/smoke/reader-long-list.scm)
chaaya_kaappi_deferred_test(smoke_reader_numeric_prefix tests/scheme/kaappi-deferred/smoke/reader-numeric-prefix.scm)
chaaya_kaappi_deferred_test(smoke_reader_token_validation tests/scheme/kaappi-deferred/smoke/reader-token-validation.scm)
chaaya_kaappi_deferred_test(smoke_record_gc_roots tests/scheme/kaappi-deferred/smoke/record-gc-roots.scm)
chaaya_kaappi_deferred_test(smoke_record_set_gc tests/scheme/kaappi-deferred/smoke/record-set-gc.scm)
# Batch 7 smoke (records, hashtables, SRFI-1)
chaaya_kaappi_deferred_test(smoke_record_shadow_drop_1718 tests/scheme/kaappi-deferred/smoke/record-shadow-drop-1718.scm)
chaaya_kaappi_deferred_test(smoke_record_namespace_clean tests/scheme/kaappi-deferred/smoke/record-namespace-clean.scm)
chaaya_kaappi_deferred_test(smoke_record_type_begin tests/scheme/kaappi-deferred/smoke/record-type-begin.scm)
chaaya_kaappi_deferred_test(smoke_cyclic_record_printer_1713 tests/scheme/kaappi-deferred/smoke/cyclic-record-printer-1713.scm)
chaaya_kaappi_deferred_test(smoke_hash_table_update_1182 tests/scheme/kaappi-deferred/smoke/hash-table-update-1182.scm)
chaaya_kaappi_deferred_test(smoke_hashtable_insert_occupied tests/scheme/kaappi-deferred/smoke/hashtable-insert-occupied.scm)
chaaya_kaappi_deferred_test(smoke_hash_table_eof_void_key tests/scheme/kaappi-deferred/smoke/hash-table-eof-void-key.scm)
chaaya_kaappi_deferred_test(smoke_gc_hashtable_nil_key tests/scheme/kaappi-deferred/smoke/gc-hashtable-nil-key.scm)
chaaya_kaappi_deferred_test(smoke_hash_table_custom_comparator tests/scheme/kaappi-deferred/smoke/hash-table-custom-comparator.scm)
chaaya_kaappi_deferred_test(smoke_hash_table_walk_gc_1181 tests/scheme/kaappi-deferred/smoke/hash-table-walk-gc-1181.scm)
chaaya_kaappi_deferred_test(smoke_srfi1_take_drop_right_dotted tests/scheme/kaappi-deferred/smoke/srfi1-take-drop-right-dotted.scm)
chaaya_kaappi_deferred_test(smoke_record_type_check tests/scheme/kaappi-deferred/smoke/record-type-check.scm)
chaaya_kaappi_deferred_test(smoke_record_type_gc_mark tests/scheme/kaappi-deferred/smoke/record-type-gc-mark.scm)
chaaya_kaappi_deferred_test(smoke_return_wind_gc tests/scheme/kaappi-deferred/smoke/return-wind-gc.scm)
chaaya_kaappi_deferred_test(smoke_round_neg_rational_837 tests/scheme/kaappi-deferred/smoke/round-neg-rational-837.scm)
chaaya_kaappi_deferred_test(smoke_scheme_base_file_exports tests/scheme/kaappi-deferred/smoke/scheme-base-file-exports.scm)
chaaya_kaappi_deferred_test(smoke_scheme_write_exports tests/scheme/kaappi-deferred/smoke/scheme-write-exports.scm)
chaaya_kaappi_deferred_test(smoke_set_redefine_fold tests/scheme/kaappi-deferred/smoke/set-redefine-fold.scm)
chaaya_kaappi_deferred_test(smoke_srfi_feature_ids_1649 tests/scheme/kaappi-deferred/smoke/srfi-feature-ids-1649.scm)
chaaya_kaappi_deferred_test(smoke_srfi27_bignum tests/scheme/kaappi-deferred/smoke/srfi27-bignum.scm)
chaaya_kaappi_deferred_test(smoke_srfi27_default_source tests/scheme/kaappi-deferred/smoke/srfi27-default-source.scm)
chaaya_kaappi_deferred_test(smoke_string_ci_folding tests/scheme/kaappi-deferred/smoke/string-ci-folding.scm)
chaaya_kaappi_deferred_test(smoke_string_fill_validation tests/scheme/kaappi-deferred/smoke/string-fill-validation.scm)
chaaya_kaappi_deferred_test(smoke_string_foreach_mutation tests/scheme/kaappi-deferred/smoke/string-foreach-mutation.scm)
chaaya_kaappi_deferred_test(smoke_string_negative_index tests/scheme/kaappi-deferred/smoke/string-negative-index.scm)
chaaya_kaappi_deferred_test(smoke_string_to_number_prefix tests/scheme/kaappi-deferred/smoke/string-to-number-prefix.scm)
chaaya_kaappi_deferred_test(smoke_string_to_number_underscores_1724 tests/scheme/kaappi-deferred/smoke/string-to-number-underscores-1724.scm)
chaaya_kaappi_deferred_test(smoke_syntax_rules_many_vars tests/scheme/kaappi-deferred/smoke/syntax-rules-many-vars.scm)
chaaya_kaappi_deferred_test(smoke_tail_apply_arity tests/scheme/kaappi-deferred/smoke/tail-apply-arity.scm)
chaaya_kaappi_deferred_test(smoke_tail_call_stale_regs_1256 tests/scheme/kaappi-deferred/smoke/tail-call-stale-regs-1256.scm)
chaaya_kaappi_deferred_test(smoke_thread_self_join tests/scheme/kaappi-deferred/smoke/thread-self-join.scm)
chaaya_kaappi_deferred_test(smoke_types_toF64_bignum_792 tests/scheme/kaappi-deferred/smoke/types-toF64-bignum-792.scm)
chaaya_kaappi_deferred_test(smoke_u8_ready_eof tests/scheme/kaappi-deferred/smoke/u8-ready-eof.scm)
chaaya_kaappi_deferred_test(smoke_unicode_identifier_roundtrip tests/scheme/kaappi-deferred/smoke/unicode-identifier-roundtrip.scm)
chaaya_kaappi_deferred_test(smoke_vector_negative_index tests/scheme/kaappi-deferred/smoke/vector-negative-index.scm)
chaaya_kaappi_deferred_test(smoke_vector_unfold_empty_values_806 tests/scheme/kaappi-deferred/smoke/vector-unfold-empty-values-806.scm)
chaaya_kaappi_deferred_test(smoke_yield_exception_handler tests/scheme/kaappi-deferred/smoke/yield-exception-handler.scm)
# Batch 7 smoke (numeric tower + SRFI-133 vector GC)
chaaya_kaappi_deferred_test(smoke_exact_inf_nan tests/scheme/kaappi-deferred/smoke/exact-inf-nan.scm)
chaaya_kaappi_deferred_test(smoke_exact_prefix_856 tests/scheme/kaappi-deferred/smoke/exact-prefix-856.scm)
chaaya_kaappi_deferred_test(smoke_even_odd_non_integer tests/scheme/kaappi-deferred/smoke/even-odd-non-integer.scm)
chaaya_kaappi_deferred_test(smoke_identity_elim_typecheck tests/scheme/kaappi-deferred/smoke/identity-elim-typecheck.scm)
chaaya_kaappi_deferred_test(smoke_trig_log_complex tests/scheme/kaappi-deferred/smoke/trig-log-complex.scm)
chaaya_kaappi_deferred_test(smoke_mixed_bignum_flonum_841 tests/scheme/kaappi-deferred/smoke/mixed-bignum-flonum-841.scm)
chaaya_kaappi_deferred_test(smoke_number_string_rational_radix tests/scheme/kaappi-deferred/smoke/number-string-rational-radix.scm)
chaaya_kaappi_deferred_test(smoke_bignum_radix_chunk tests/scheme/kaappi-deferred/smoke/bignum-radix-chunk.scm)
chaaya_kaappi_deferred_test(smoke_vector_map_gc tests/scheme/kaappi-deferred/smoke/vector-map-gc.scm)
# Batch 8 smoke (parameterize/strings + SRFI-18 / parallel / library fixtures)
chaaya_kaappi_deferred_test(smoke_parameterize_convert_1266 tests/scheme/kaappi-deferred/smoke/parameterize-convert-1266.scm)
chaaya_kaappi_deferred_test(smoke_trim_whitespace tests/scheme/kaappi-deferred/smoke/trim-whitespace.scm)
chaaya_kaappi_deferred_test(smoke_thread_library_gc tests/scheme/kaappi-deferred/smoke/thread-library-gc.scm)
chaaya_kaappi_deferred_test(smoke_thread_frame_gc_1156 tests/scheme/kaappi-deferred/smoke/thread-frame-gc-1156.scm)
chaaya_kaappi_deferred_test(smoke_library_thread_cross_call_1479 tests/scheme/kaappi-deferred/smoke/library-thread-cross-call-1479.scm)
chaaya_kaappi_deferred_test(smoke_library_thread_named_helper_1520 tests/scheme/kaappi-deferred/smoke/library-thread-named-helper-1520.scm)
chaaya_kaappi_deferred_test(smoke_kaappi_parallel_map tests/scheme/kaappi-deferred/smoke/kaappi-parallel-map.scm)
chaaya_kaappi_deferred_test(smoke_include_ci_1141 tests/scheme/kaappi-deferred/smoke/include-ci-1141.scm)
chaaya_kaappi_deferred_test(smoke_condexpand_include_lib_868_879 tests/scheme/kaappi-deferred/smoke/condexpand-include-lib-868-879.scm)
chaaya_kaappi_deferred_test(smoke_read_incomplete_datum tests/scheme/kaappi-deferred/smoke/read-incomplete-datum.scm)
chaaya_kaappi_deferred_test(smoke_reader_peculiar_idents tests/scheme/kaappi-deferred/smoke/reader-peculiar-idents.scm)
chaaya_kaappi_deferred_test(smoke_eval_environment tests/scheme/kaappi-deferred/smoke/eval-environment.scm)
chaaya_kaappi_deferred_test(smoke_case_arrow_shadowing tests/scheme/kaappi-deferred/smoke/case-arrow-shadowing.scm)
chaaya_kaappi_deferred_test(smoke_eval_macro_leak_1269 tests/scheme/kaappi-deferred/smoke/eval-macro-leak-1269.scm)
chaaya_kaappi_deferred_test(smoke_560_record_body tests/scheme/kaappi-deferred/smoke/560-record-body.scm)
# Batch 9 smoke (language/library parity phase 2/5)
chaaya_kaappi_deferred_test(smoke_file_info_blocks tests/scheme/kaappi-deferred/smoke/file-info-blocks.scm)
chaaya_kaappi_deferred_test(smoke_mutex_lock_false_owner tests/scheme/kaappi-deferred/smoke/mutex-lock-false-owner.scm)
# nested-wait-under-sleep-dirty-snapshot-1490.scm: passes in isolation but
# times out under parallel ctest load (contended scheduler timing) — a
# cross-thread scheduler race, not a quick fix; left unwired (see
# docs/dev/scheme-tests.md).

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

add_test(NAME cli_diagnostics_json
  COMMAND ${CMAKE_COMMAND}
    -DCHAAYA=$<TARGET_FILE:chaaya>
    -DINPUT=${CMAKE_SOURCE_DIR}/tests/cli/check_compile_error.scm
    -P ${CMAKE_SOURCE_DIR}/tests/cli/run_diagnostics_json.cmake
)

add_test(NAME cli_explain
  COMMAND ${CMAKE_COMMAND}
    -DCHAAYA=$<TARGET_FILE:chaaya>
    -P ${CMAKE_SOURCE_DIR}/tests/cli/run_explain.cmake
)

add_test(NAME cli_ir
  COMMAND ${CMAKE_COMMAND}
    -DCHAAYA=$<TARGET_FILE:chaaya>
    -DINPUT=${CMAKE_SOURCE_DIR}/tests/cli/check_ok.scm
    -P ${CMAKE_SOURCE_DIR}/tests/cli/run_ir.cmake
)

add_test(NAME cli_doctor
  COMMAND ${CMAKE_COMMAND}
    -DCHAAYA=$<TARGET_FILE:chaaya>
    -P ${CMAKE_SOURCE_DIR}/tests/cli/run_doctor.cmake
)

add_test(NAME cli_cache_status
  COMMAND ${CMAKE_COMMAND}
    -DCHAAYA=$<TARGET_FILE:chaaya>
    -P ${CMAKE_SOURCE_DIR}/tests/cli/run_cache_status.cmake
)

add_test(NAME cli_cache_roundtrip
  COMMAND ${CMAKE_COMMAND}
    -DCHAAYA=$<TARGET_FILE:chaaya>
    -DINPUT=${CMAKE_SOURCE_DIR}/tests/cli/cache_roundtrip.scm
    -P ${CMAKE_SOURCE_DIR}/tests/cli/run_cache_roundtrip.cmake
)

add_test(NAME cli_fmt_check
  COMMAND ${CMAKE_COMMAND}
    -DCHAAYA=$<TARGET_FILE:chaaya>
    -DINPUT=${CMAKE_SOURCE_DIR}/tests/cli/fmt_ok.scm
    -P ${CMAKE_SOURCE_DIR}/tests/cli/run_fmt_check.cmake
)

add_test(NAME cli_test_json
  COMMAND ${CMAKE_COMMAND}
    -DCHAAYA=$<TARGET_FILE:chaaya>
    -DLIB_PATH=${CMAKE_SOURCE_DIR}/lib
    -DPASS_FILE=${CMAKE_SOURCE_DIR}/tests/cli/test_srfi64_pass.scm
    -DFAIL_FILE=${CMAKE_SOURCE_DIR}/tests/cli/test_srfi64_fail.scm
    -P ${CMAKE_SOURCE_DIR}/tests/cli/run_test_json.cmake
)

add_test(NAME cli_compile_mvp
  COMMAND ${CMAKE_COMMAND}
    -DCHAAYA=$<TARGET_FILE:chaaya>
    -DINPUT=${CMAKE_SOURCE_DIR}/tests/cli/check_ok.scm
    -DOUTPUT=${CMAKE_BINARY_DIR}/cli_compile_out
    -P ${CMAKE_SOURCE_DIR}/tests/cli/expect_nyi_compile.cmake
)
set_tests_properties(cli_compile_mvp PROPERTIES DEPENDS chaaya_rt)

add_test(NAME cli_native_mvp
  COMMAND ${CMAKE_COMMAND}
    -DCHAAYA=$<TARGET_FILE:chaaya>
    -DINPUT=${CMAKE_SOURCE_DIR}/tests/cli/native_ok.scm
    -P ${CMAKE_SOURCE_DIR}/tests/cli/run_native_exec.cmake
)

add_test(NAME cli_native_defines
  COMMAND ${CMAKE_COMMAND}
    -DCHAAYA=$<TARGET_FILE:chaaya>
    -DINPUT=${CMAKE_SOURCE_DIR}/tests/cli/check_ok.scm
    -DOUTPUT=${CMAKE_BINARY_DIR}/cli_native_defines_bin
    -P ${CMAKE_SOURCE_DIR}/tests/cli/run_native_defines.cmake
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

add_test(NAME cli_lsp_didopen_diagnostics
  COMMAND ${CMAKE_COMMAND}
    -DCHAAYA=$<TARGET_FILE:chaaya>
    -P ${CMAKE_SOURCE_DIR}/tests/cli/run_lsp_diagnostics.cmake
)

add_test(NAME cli_lsp_completion
  COMMAND ${CMAKE_COMMAND}
    -DCHAAYA=$<TARGET_FILE:chaaya>
    -P ${CMAKE_SOURCE_DIR}/tests/cli/run_lsp_completion.cmake
)

add_test(NAME cli_stdin
  COMMAND ${CMAKE_COMMAND}
    -DCHAAYA=$<TARGET_FILE:chaaya>
    -P ${CMAKE_SOURCE_DIR}/tests/cli/run_stdin.cmake
)

install(TARGETS chaaya RUNTIME DESTINATION bin)
