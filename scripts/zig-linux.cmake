# CMake toolchain: x86-64 Linux, glibc 2.36, with zig cc, over the sysroot that
# $FUNK_SYSROOT names (scripts/linux-sysroot.sh). See docs/dev/cross-build.md.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

find_program(ZIG zig REQUIRED)
set(CMAKE_C_COMPILER ${ZIG} cc -target x86_64-linux-gnu.2.36)
set(CMAKE_CXX_COMPILER ${ZIG} c++ -target x86_64-linux-gnu.2.36)
# the host's ar and ranlib may not read ELF objects (macOS's do not)
foreach(lang C CXX)
  set(CMAKE_${lang}_ARCHIVE_CREATE "${ZIG} ar qc <TARGET> <LINK_FLAGS> <OBJECTS>")
  set(CMAKE_${lang}_ARCHIVE_APPEND "${ZIG} ar q <TARGET> <LINK_FLAGS> <OBJECTS>")
  set(CMAKE_${lang}_ARCHIVE_FINISH "${ZIG} ranlib <TARGET>")
endforeach()

if(NOT DEFINED ENV{FUNK_SYSROOT})
  message(FATAL_ERROR "zig-linux.cmake: set FUNK_SYSROOT to a sysroot from scripts/linux-sysroot.sh")
endif()
set(CMAKE_FIND_ROOT_PATH $ENV{FUNK_SYSROOT})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
set(CMAKE_LIBRARY_ARCHITECTURE x86_64-linux-gnu)
set(CMAKE_SKIP_BUILD_RPATH ON) # the sysroot's paths mean nothing on the target
