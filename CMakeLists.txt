cmake_minimum_required(VERSION 3.20)
project(funk VERSION 0.6.0 LANGUAGES C)

# funk is one executable over libfunk. The runtime (src/rt.c, with what it includes)
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

# libfunk: the compiler, interpreter, C backend and runtime, with the api
# in src/libfunk.h. Internal and static; the command line links it.
add_library(libfunk STATIC
  src/driver.c src/util.c src/lex.c src/parse.c src/check.c src/elab.c src/fuse.c
  src/resolve.c src/qualify.c src/interp.c src/cgen.c src/ffi.c src/rt.c
  ${CMAKE_CURRENT_BINARY_DIR}/embedded.c)
set_target_properties(libfunk PROPERTIES OUTPUT_NAME funk)
target_include_directories(libfunk INTERFACE src)
target_compile_definitions(libfunk PRIVATE FUNK_VERSION="${PROJECT_VERSION}")
target_compile_options(libfunk PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(libfunk PUBLIC Threads::Threads m ${CMAKE_DL_LIBS})

add_executable(funk cli/main.c)
target_compile_options(funk PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(funk PRIVATE libfunk)
set_target_properties(funk PROPERTIES ENABLE_EXPORTS ON) # the interpreter's foreign glue calls the runtime

# the reload threshold of docs/dev/patterns.md, over libfunk.h; a test runs it
add_executable(reload bench/reload.c)
target_compile_options(reload PRIVATE -Wall -Wextra)
target_link_libraries(reload PRIVATE libfunk)

# The graph player for compiled kernels (hosts/graph, docs/dev/audio.md, D);
# static and position-independent, so foreign-import glue can link it.
add_library(funkgraph STATIC hosts/graph/graph.c)
set_target_properties(funkgraph PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_options(funkgraph PRIVATE -Wall -Wextra)
target_include_directories(funkgraph PUBLIC hosts/graph)

# lib/kernels.fk, compiled for lib/dsp.fk's signals
set(FUNK_DSP_LIB ${CMAKE_CURRENT_BINARY_DIR}/funkdsp.so)
add_custom_command(OUTPUT ${FUNK_DSP_LIB}
  COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/tools/kernels.sh $<TARGET_FILE:funk> ${CMAKE_CURRENT_SOURCE_DIR}/lib/kernels.fk ${FUNK_DSP_LIB}
  DEPENDS funk lib/kernels.fk tools/kernels.sh)
add_custom_target(funkdsp ALL DEPENDS ${FUNK_DSP_LIB})
# the cost of graphs, kernels and voices (docs/dev/audio.md, D, item 5):
#   build/dspbench build/funkdsp.so [BLOCK]
add_executable(dspbench bench/dsp.c)
target_compile_options(dspbench PRIVATE -Wall -Wextra)
target_link_libraries(dspbench PRIVATE funkgraph ${CMAKE_DL_LIBS} m)

# funk-midi: plays a pattern to MIDI, live (hosts/midi/main.c). Without
# libremidi it can only print, which the tests use. FUNK_LIBREMIDI_DIR is a
# libremidi source tree, e.g. midi-langs/thirdparty/libremidi. With
# --audio it plays a graph of kernels too: through miniaudio if
# FUNK_MINIAUDIO_DIR holds miniaudio.h, else only on the virtual clock.
set(FUNK_LIBREMIDI_DIR "" CACHE PATH "libremidi source, for funk-midi's MIDI output")
set(FUNK_MINIAUDIO_DIR "" CACHE PATH "the directory of miniaudio.h, for funk-midi's audio output")
add_executable(funk-midi hosts/midi/main.c)
target_compile_options(funk-midi PRIVATE -Wall -Wextra)
target_compile_definitions(funk-midi PRIVATE FUNK_LIB_DIR="${CMAKE_CURRENT_SOURCE_DIR}/lib"
  FUNK_DSP_LIB="${FUNK_DSP_LIB}" FUNK_GRAPH_INCLUDE="${CMAKE_CURRENT_SOURCE_DIR}/hosts/graph")
target_link_libraries(funk-midi PRIVATE libfunk funkgraph)
set_target_properties(funk-midi PROPERTIES ENABLE_EXPORTS ON) # graph.fk's glue calls the runtime and funkgraph
add_dependencies(funk-midi funkdsp)
if(FUNK_MINIAUDIO_DIR)
  target_sources(funk-midi PRIVATE hosts/midi/miniaudio.c)
  target_include_directories(funk-midi PRIVATE ${FUNK_MINIAUDIO_DIR})
  target_compile_definitions(funk-midi PRIVATE HAVE_MINIAUDIO)
  set_source_files_properties(hosts/midi/miniaudio.c PROPERTIES COMPILE_OPTIONS -w)
  if(APPLE)
    target_link_libraries(funk-midi PRIVATE "-framework CoreFoundation" "-framework CoreAudio" "-framework AudioToolbox")
  endif()
endif()
if(FUNK_LIBREMIDI_DIR)
  enable_language(CXX)
  set(LIBREMIDI_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(LIBREMIDI_TESTS OFF CACHE BOOL "" FORCE)
  set(LIBREMIDI_NO_PIPEWIRE ON CACHE BOOL "" FORCE)
  set(LIBREMIDI_NO_WINMIDI ON CACHE BOOL "" FORCE)
  add_subdirectory(${FUNK_LIBREMIDI_DIR} libremidi EXCLUDE_FROM_ALL)
  target_compile_definitions(funk-midi PRIVATE HAVE_LIBREMIDI)
  target_link_libraries(funk-midi PRIVATE libremidi)
endif()

if(FUNK_SANITIZE)
  foreach(t libfunk funk reload funk-midi)
    target_compile_options(${t} PRIVATE -fsanitize=${FUNK_SANITIZE} -fno-sanitize-recover=all)
  endforeach()
  foreach(t funk reload funk-midi)
    target_link_options(${t} PRIVATE -fsanitize=${FUNK_SANITIZE})
  endforeach()
endif()

# Included by another project (midi-langs' funk-midi plugin), funk adds its
# targets only: not its install rule or its tests.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  install(TARGETS funk RUNTIME DESTINATION bin)
  enable_testing()
  add_subdirectory(tests)
endif()
