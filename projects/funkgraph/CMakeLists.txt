# The graph player for compiled kernels (docs/dev/audio.md, D); static and
# position-independent, so foreign-import glue can link it.
# thirdparty/mdapiano's samples, built in (table.h's funk_table_builtin)
set(MDAPIANO_C ${CMAKE_CURRENT_BINARY_DIR}/mdapiano.c)
set(MDAPIANO_C ${MDAPIANO_C} PARENT_SCOPE) # tests/ compiles it with table.c
add_custom_command(OUTPUT ${MDAPIANO_C}
  COMMAND ${CMAKE_COMMAND} -DIN=${funk_SOURCE_DIR}/thirdparty/mdapiano/mdapiano.wav -DOUT=${MDAPIANO_C} -DNAME=funk_mdapiano
    -P ${CMAKE_CURRENT_SOURCE_DIR}/embed.cmake
  DEPENDS ${funk_SOURCE_DIR}/thirdparty/mdapiano/mdapiano.wav ${CMAKE_CURRENT_SOURCE_DIR}/embed.cmake
  VERBATIM)
add_library(funkgraph STATIC graph.c events.c voices.c table.c controls.c ${MDAPIANO_C})
target_link_libraries(funkgraph PUBLIC Threads::Threads)
set_target_properties(funkgraph PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_options(funkgraph PRIVATE -Wall -Wextra)
target_include_directories(funkgraph PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# lib/kernels.fk, compiled for lib/dsp.fk's signals, into FUNK_DSP_LIB. The C's
# first line, /* link: -lm */, names its libraries; kernels.rsp holds them.
set(KERNELS_C ${CMAKE_CURRENT_BINARY_DIR}/kernels.c)
set(KERNELS_RSP ${CMAKE_CURRENT_BINARY_DIR}/kernels.rsp)
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/link-words.cmake [[
file(STRINGS ${C} line LIMIT_COUNT 1)
string(REGEX REPLACE "^/\\* link: (.*) \\*/$" "\\1" words "${line}")
if(words STREQUAL line)
  set(words "")
endif()
file(WRITE ${RSP} "${words}\n")
]])
add_custom_command(OUTPUT ${KERNELS_C} ${KERNELS_RSP}
  COMMAND funk_tool emit-c --kernels ${funk_SOURCE_DIR}/lib/kernels.fk -o ${KERNELS_C}
  COMMAND ${CMAKE_COMMAND} -DC=${KERNELS_C} -DRSP=${KERNELS_RSP} -P ${CMAKE_CURRENT_BINARY_DIR}/link-words.cmake
  DEPENDS funk_tool ${funk_SOURCE_DIR}/lib/kernels.fk
  VERBATIM)
add_library(funkdsp MODULE ${KERNELS_C})
target_compile_options(funkdsp PRIVATE -O2) # in every build type: the audio thread runs it
# the libraries go after the objects; zig cc takes @file from its driver, not -Wl,@file.
# This directory's only other target, funkgraph, is static.
set(CMAKE_C_CREATE_SHARED_MODULE "${CMAKE_C_CREATE_SHARED_MODULE} @${KERNELS_RSP}")
set_target_properties(funkdsp PROPERTIES PREFIX "" SUFFIX ".so" LINK_DEPENDS ${KERNELS_RSP}
  LIBRARY_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR})
