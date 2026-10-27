cmake_minimum_required(VERSION 3.20)
project(funk VERSION 0.4.0 LANGUAGES C)

# funk is one executable. The runtime (src/rt.c, with what it includes)
# and the prelude are embedded in it as C strings: compiled programs get
# the runtime pasted into their generated C, so they need no library.

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(FUNK_SANITIZE "" CACHE STRING "Sanitizers for funk itself, e.g. undefined or address")

find_package(Threads REQUIRED)

add_executable(embed tools/embed.c)

set(RUNTIME_SOURCES src/rt.c src/rt.h src/prims.def)
add_custom_command(
  OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/embedded.c
  COMMAND embed -o ${CMAKE_CURRENT_BINARY_DIR}/embedded.c
          fk_rt_src ${CMAKE_CURRENT_SOURCE_DIR}/src/rt.c
          fk_rth_src ${CMAKE_CURRENT_SOURCE_DIR}/src/rt.h
          fk_prelude_src ${CMAKE_CURRENT_SOURCE_DIR}/lib/prelude.fk
  DEPENDS embed ${RUNTIME_SOURCES} lib/prelude.fk
  COMMENT "Embedding the runtime and the prelude"
  VERBATIM)

add_executable(funk
  src/main.c src/util.c src/lex.c src/parse.c src/check.c src/elab.c src/fuse.c
  src/resolve.c src/interp.c src/cgen.c src/ffi.c src/rt.c
  ${CMAKE_CURRENT_BINARY_DIR}/embedded.c)
target_compile_definitions(funk PRIVATE FUNK_VERSION="${PROJECT_VERSION}")
target_compile_options(funk PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(funk PRIVATE Threads::Threads m ${CMAKE_DL_LIBS})
set_target_properties(funk PROPERTIES ENABLE_EXPORTS ON) # the interpreter's foreign glue calls the runtime
if(FUNK_SANITIZE)
  target_compile_options(funk PRIVATE -fsanitize=${FUNK_SANITIZE} -fno-sanitize-recover=all)
  target_link_options(funk PRIVATE -fsanitize=${FUNK_SANITIZE})
endif()

install(TARGETS funk RUNTIME DESTINATION bin)

enable_testing()
add_test(NAME funk-suite COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/tests/run.sh)
set_tests_properties(funk-suite PROPERTIES ENVIRONMENT "FUNK=$<TARGET_FILE:funk>")
