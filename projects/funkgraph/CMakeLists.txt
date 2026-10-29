# The graph player for compiled kernels (docs/dev/audio.md, D); static and
# position-independent, so foreign-import glue can link it.
add_library(funkgraph STATIC graph.c)
set_target_properties(funkgraph PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_options(funkgraph PRIVATE -Wall -Wextra)
target_include_directories(funkgraph PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# lib/kernels.fk, compiled for lib/dsp.fk's signals, into FUNK_DSP_LIB
add_custom_command(OUTPUT ${FUNK_DSP_LIB}
  COMMAND sh ${funk_SOURCE_DIR}/tools/kernels.sh $<TARGET_FILE:funk> ${funk_SOURCE_DIR}/lib/kernels.fk ${FUNK_DSP_LIB}
  DEPENDS funk ${funk_SOURCE_DIR}/lib/kernels.fk ${funk_SOURCE_DIR}/tools/kernels.sh)
add_custom_target(funkdsp ALL DEPENDS ${FUNK_DSP_LIB})
