# funk is one executable over libfunk. The runtime (src/rt.c, with what it includes)
# and the prelude are embedded in it as C strings: compiled programs get
# the runtime pasted into their generated C, so they need no library.

add_executable(embed tools/embed.c)

set(RUNTIME_SOURCES src/rt.c src/rt.h src/prims.def)
add_custom_command(
  OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/embedded.c
  COMMAND embed -o ${CMAKE_CURRENT_BINARY_DIR}/embedded.c
          fk_rt_src ${CMAKE_CURRENT_SOURCE_DIR}/src/rt.c
          fk_rth_src ${CMAKE_CURRENT_SOURCE_DIR}/src/rt.h
          fk_prelude_src ${funk_SOURCE_DIR}/lib/prelude.fk
  DEPENDS embed ${RUNTIME_SOURCES} ${funk_SOURCE_DIR}/lib/prelude.fk
  COMMENT "Embedding the runtime and the prelude"
  VERBATIM)

# libfunk: the compiler, interpreter, C backend and runtime, with the api
# in src/libfunk.h. Static; the command line links it, and so may a host.
# It exports only libfunk.h's funk_*; position-independent, for hosts that
# are plugins.
add_library(libfunk STATIC
  src/driver.c src/util.c src/lex.c src/parse.c src/check.c src/elab.c src/fuse.c
  src/resolve.c src/qualify.c src/interp.c src/cgen.c src/ffi.c src/rt.c
  ${CMAKE_CURRENT_BINARY_DIR}/embedded.c)
set_target_properties(libfunk PROPERTIES OUTPUT_NAME funk C_VISIBILITY_PRESET hidden
  POSITION_INDEPENDENT_CODE ON PUBLIC_HEADER src/libfunk.h)
target_include_directories(libfunk INTERFACE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src> $<INSTALL_INTERFACE:include>)
# FK_SESSIONS: the runtime's state per session, found from the stack (src/rt.h);
# compiled programs paste src/rt.c without it, as one session
target_compile_definitions(libfunk PRIVATE FUNK_VERSION="${PROJECT_VERSION}" FK_SESSIONS)
target_compile_options(libfunk PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(libfunk PUBLIC Threads::Threads m ${CMAKE_DL_LIBS})

add_executable(funk cli/main.c)
target_compile_options(funk PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(funk PRIVATE libfunk)

# funk, and find_package(funk) giving funk::libfunk; not when funk is
# included by another project
if(CMAKE_SOURCE_DIR STREQUAL funk_SOURCE_DIR)
  install(TARGETS funk RUNTIME DESTINATION bin)
  install(TARGETS libfunk EXPORT funkTargets ARCHIVE DESTINATION lib PUBLIC_HEADER DESTINATION include)
  install(EXPORT funkTargets NAMESPACE funk:: DESTINATION lib/cmake/funk)
  include(CMakePackageConfigHelpers)
  write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/funkConfigVersion.cmake COMPATIBILITY ExactVersion)
  install(FILES cmake/funkConfig.cmake ${CMAKE_CURRENT_BINARY_DIR}/funkConfigVersion.cmake DESTINATION lib/cmake/funk)
endif()
