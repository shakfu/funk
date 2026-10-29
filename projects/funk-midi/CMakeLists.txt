# funk-midi: plays a pattern to MIDI, live (main.c). Without libremidi it
# can only print, which the tests use; FUNK_LIBREMIDI builds the vendored
# thirdparty/libremidi. With --audio it plays a graph of kernels too:
# through the vendored thirdparty/miniaudio with FUNK_MINIAUDIO, else only
# on the virtual clock.
add_executable(funk-midi main.c)
target_compile_options(funk-midi PRIVATE -Wall -Wextra)
target_compile_definitions(funk-midi PRIVATE FUNK_LIB_DIR="${funk_SOURCE_DIR}/lib"
  FUNK_DSP_LIB="${FUNK_DSP_LIB}" FUNK_GRAPH_INCLUDE="${funk_SOURCE_DIR}/projects/funkgraph")
target_link_libraries(funk-midi PRIVATE libfunk funkgraph)
add_dependencies(funk-midi funkdsp)
if(FUNK_MINIAUDIO)
  set(MINIAUDIO ${funk_SOURCE_DIR}/thirdparty/miniaudio)
  target_sources(funk-midi PRIVATE ${MINIAUDIO}/miniaudio.c)
  target_include_directories(funk-midi PRIVATE ${MINIAUDIO})
  target_compile_definitions(funk-midi PRIVATE HAVE_MINIAUDIO)
  set_source_files_properties(${MINIAUDIO}/miniaudio.c PROPERTIES COMPILE_OPTIONS -w)
  if(APPLE)
    target_link_libraries(funk-midi PRIVATE "-framework CoreFoundation" "-framework CoreAudio" "-framework AudioToolbox")
  endif()
endif()
if(FUNK_LIBREMIDI)
  enable_language(CXX)
  set(LIBREMIDI_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(LIBREMIDI_TESTS OFF CACHE BOOL "" FORCE)
  set(LIBREMIDI_NO_PIPEWIRE ON CACHE BOOL "" FORCE)
  set(LIBREMIDI_NO_WINMIDI ON CACHE BOOL "" FORCE)
  add_subdirectory(${funk_SOURCE_DIR}/thirdparty/libremidi ${PROJECT_BINARY_DIR}/libremidi EXCLUDE_FROM_ALL)
  target_compile_definitions(funk-midi PRIVATE HAVE_LIBREMIDI)
  target_link_libraries(funk-midi PRIVATE libremidi)
endif()
