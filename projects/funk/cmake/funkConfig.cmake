# find_package(funk): the target funk::libfunk (CMakeLists.txt)
include(CMakeFindDependencyMacro)
find_dependency(Threads)
include(${CMAKE_CURRENT_LIST_DIR}/funkTargets.cmake)
