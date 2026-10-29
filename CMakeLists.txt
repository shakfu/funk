cmake_minimum_required(VERSION 3.20)
project(funk VERSION 0.6.0 LANGUAGES C)

# One build of every project under projects/:
#   funk       the language: libfunk and the command line
#   funkgraph  the graph player for compiled kernels, and funkdsp.so
#   funk-midi  plays patterns to MIDI, and graphs to audio, live

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}) # build/funk, build/funk-midi

set(FUNK_SANITIZE "" CACHE STRING "Sanitizers for funk itself, e.g. undefined or address")
option(FUNK_LIBREMIDI "funk-midi's MIDI output, with thirdparty/libremidi" OFF)
option(FUNK_MINIAUDIO "funk-midi's audio output, with thirdparty/miniaudio" ON)
set(FUNK_DSP_LIB ${PROJECT_BINARY_DIR}/funkdsp.so) # lib/kernels.fk, compiled

find_package(Threads REQUIRED)

add_subdirectory(projects/funk)
add_subdirectory(projects/funkgraph)
add_subdirectory(projects/funk-midi)

# the reload threshold of docs/dev/patterns.md, over libfunk.h; a test runs it
add_executable(reload bench/reload.c)
target_compile_options(reload PRIVATE -Wall -Wextra)
target_link_libraries(reload PRIVATE libfunk)
# the cost of graphs, kernels and voices (docs/dev/audio.md, D, item 5):
#   build/dspbench build/funkdsp.so [BLOCK]
add_executable(dspbench bench/dsp.c)
target_compile_options(dspbench PRIVATE -Wall -Wextra)
target_link_libraries(dspbench PRIVATE funkgraph ${CMAKE_DL_LIBS} m)

if(FUNK_SANITIZE)
  foreach(t libfunk funk reload funk-midi)
    target_compile_options(${t} PRIVATE -fsanitize=${FUNK_SANITIZE} -fno-sanitize-recover=all)
  endforeach()
  foreach(t funk reload funk-midi)
    target_link_options(${t} PRIVATE -fsanitize=${FUNK_SANITIZE})
  endforeach()
  target_link_options(libfunk INTERFACE -fsanitize=${FUNK_SANITIZE}) # for hosts, the installed package's too
endif()

# Included by another project (midi-langs' funk-midi plugin), funk adds its
# targets only: not its install rules (projects/funk) or its tests.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  enable_testing()
  add_subdirectory(tests)
endif()
