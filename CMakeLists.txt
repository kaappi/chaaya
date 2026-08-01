cmake_minimum_required(VERSION 3.20)
project(chaaya VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

option(CHAAYA_WARNINGS_AS_ERRORS "Treat warnings as errors" ON)
option(CHAAYA_USE_LINENOISE "Use linenoise for interactive REPL (POSIX)" ON)

add_library(chaaya_core STATIC
  src/value.c
  src/gc.c
  src/reader.c
  src/printer.c
  src/compiler.c
  src/vm.c
  src/prim_core.c
)

target_include_directories(chaaya_core
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_options(chaaya_core PRIVATE
  -Wall -Wextra -Wpedantic
  $<$<BOOL:${CHAAYA_WARNINGS_AS_ERRORS}>:-Werror>
)

add_executable(chaaya src/main.c)
target_link_libraries(chaaya PRIVATE chaaya_core)
target_compile_options(chaaya PRIVATE
  -Wall -Wextra -Wpedantic
  $<$<BOOL:${CHAAYA_WARNINGS_AS_ERRORS}>:-Werror>
)

# linenoise is termios/POSIX-only — skip on Windows
if(CHAAYA_USE_LINENOISE AND NOT WIN32)
  add_library(linenoise STATIC third_party/linenoise/linenoise.c)
  target_include_directories(linenoise PUBLIC third_party/linenoise)
  # Vendored code: keep warnings visible but not fatal
  target_compile_options(linenoise PRIVATE -Wall -Wno-unused-parameter)
  target_link_libraries(chaaya PRIVATE linenoise)
  target_compile_definitions(chaaya PRIVATE CHAAYA_HAS_LINENOISE=1)
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
chaaya_add_test(test_vm tests/c/test_vm.c)
chaaya_add_test(test_primitives tests/c/test_primitives.c)

# Scheme file tests: compare chaaya stdout to *.expected
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

install(TARGETS chaaya RUNTIME DESTINATION bin)
