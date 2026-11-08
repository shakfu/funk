# The graph player for compiled kernels (docs/dev/audio.md, D); static and
# position-independent, so foreign-import glue can link it.
add_library(funkgraph STATIC graph.c voices.c table.c controls.c)
target_link_libraries(funkgraph PUBLIC Threads::Threads)
set_target_properties(funkgraph PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_options(funkgraph PRIVATE -Wall -Wextra)
target_include_directories(funkgraph PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# lib/kernels.fk, compiled for lib/dsp.fk's signals, into FUNK_DSP_LIB, by the build's
# compiler: $CC at build time may be another, or unset in a cross build
add_custom_command(OUTPUT ${FUNK_DSP_LIB}
  COMMAND ${CMAKE_COMMAND} -E env "CC=${CMAKE_C_COMPILER} ${CMAKE_C_COMPILER_ARG1}"
          sh ${funk_SOURCE_DIR}/scripts/kernels.sh $<TARGET_FILE:funk_tool> ${funk_SOURCE_DIR}/lib/kernels.fk ${FUNK_DSP_LIB}
  DEPENDS funk_tool ${funk_SOURCE_DIR}/lib/kernels.fk ${funk_SOURCE_DIR}/scripts/kernels.sh
  VERBATIM)
add_custom_target(funkdsp ALL DEPENDS ${FUNK_DSP_LIB})
