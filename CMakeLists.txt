cmake_minimum_required(VERSION 3.20)
project(funk VERSION 0.8.0 LANGUAGES C)

# One build of every project under projects/:
#   funk       the language: libfunk and the command line
#   funkgraph  the graph player for compiled kernels, and funkdsp.so
#   funk-midi  plays patterns to MIDI, and graphs to audio, live
#   clap       a CLAP plugin framework, from midi-langs (FUNK_CLAP)
#   funk-clap  funk-midi as a CLAP plugin, on clap (FUNK_CLAP)

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

option(FUNK_CLAP "the funk-midi CLAP plugin, with thirdparty/{clap,pugl,imgui,libremidi}" OFF)
if(FUNK_CLAP AND UNIX AND NOT APPLE)
  find_package(X11)
  find_package(OpenGL COMPONENTS OpenGL GLX)
  if(NOT X11_FOUND OR NOT OpenGL_OpenGL_FOUND OR NOT OpenGL_GLX_FOUND)
    message(WARNING "FUNK_CLAP off: the plugin needs X11 and OpenGL headers (libx11-dev libgl-dev)")
    set(FUNK_CLAP OFF)
  endif()
endif()
if(FUNK_CLAP)
  set(CMAKE_POSITION_INDEPENDENT_CODE ON) # static libraries in the plugin
endif()

find_package(Threads REQUIRED)

if(FUNK_LIBREMIDI OR FUNK_CLAP)
  enable_language(CXX)
  set(LIBREMIDI_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(LIBREMIDI_TESTS OFF CACHE BOOL "" FORCE)
  set(LIBREMIDI_NO_PIPEWIRE ON CACHE BOOL "" FORCE)
  set(LIBREMIDI_NO_WINMIDI ON CACHE BOOL "" FORCE)
  add_subdirectory(thirdparty/libremidi ${PROJECT_BINARY_DIR}/libremidi EXCLUDE_FROM_ALL)
endif()

add_subdirectory(projects/funk)
add_subdirectory(projects/funkgraph)
add_subdirectory(projects/funk-midi)
if(FUNK_CLAP)
  set(CMAKE_CXX_STANDARD 17)
  if(APPLE)
    enable_language(OBJC) # thirdparty/pugl
  endif()
  add_subdirectory(projects/clap)
  add_subdirectory(projects/funk-clap)
endif()

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
  set(san_flags -fsanitize=${FUNK_SANITIZE} -fno-sanitize-recover=all)
  # ASan aligns a VLA to 32 bytes, and the padding is never written: FUNK_GCSTRESS's
  # check read a stale pointer in eval's as a root
  if(FUNK_SANITIZE MATCHES "address" AND CMAKE_C_COMPILER_ID MATCHES "Clang")
    list(APPEND san_flags "SHELL:-mllvm -asan-instrument-dynamic-allocas=0")
  elseif(FUNK_SANITIZE MATCHES "address")
    list(APPEND san_flags --param=asan-instrument-allocas=0)
  endif()
  foreach(t libfunk funk reload funk-midi)
    target_compile_options(${t} PRIVATE ${san_flags})
  endforeach()
  foreach(t funk reload funk-midi)
    target_link_options(${t} PRIVATE -fsanitize=${FUNK_SANITIZE})
  endforeach()
  target_link_options(libfunk INTERFACE -fsanitize=${FUNK_SANITIZE}) # for hosts, the installed package's too
endif()

# Included by another project, funk adds its targets only: not its install
# rules (projects/funk) or its tests.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  enable_testing()
  add_subdirectory(tests)
endif()
