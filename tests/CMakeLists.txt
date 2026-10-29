# The test suite, one ctest test per check; tests/check.sh runs each and
# documents what they check. `make test` runs them in parallel; one by
# name: ctest --test-dir build -R gcdeep. Labels: run, example, game, error,
# repl, ffi, stress, cli, lib, hosts, embed, kernels.

set(CHECK sh ${PROJECT_SOURCE_DIR}/tests/check.sh)

# funk_test(NAME LABELS ARGS...)
function(funk_test name labels)
  add_test(NAME ${name} COMMAND ${CHECK} ${ARGN})
  set_tests_properties(${name} PROPERTIES ENVIRONMENT "FUNK=$<TARGET_FILE:funk>" LABELS "${labels}" TIMEOUT 300)
endfunction()

file(GLOB runs RELATIVE ${PROJECT_SOURCE_DIR} CONFIGURE_DEPENDS
     ${PROJECT_SOURCE_DIR}/tests/run/*.fk ${PROJECT_SOURCE_DIR}/examples/*.fk)
foreach(f ${runs})
  get_filename_component(n ${f} NAME_WE)
  get_filename_component(d ${f} DIRECTORY)
  set(label run)
  if(d STREQUAL "examples")
    set(label example)
  endif()
  foreach(mode interp compiled types wall)
    funk_test(${label}/${n}/${mode} ${label} run ${f} ${mode})
  endforeach()
  foreach(mode reuse fuse)
    if(EXISTS ${PROJECT_SOURCE_DIR}/${d}/${n}.${mode})
      funk_test(${label}/${n}/${mode} ${label} run ${f} ${mode})
    endif()
  endforeach()
endforeach()

# modules in lib/, imported through -M lib
file(GLOB libs RELATIVE ${PROJECT_SOURCE_DIR} CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/tests/lib/*.fk)
foreach(f ${libs})
  get_filename_component(n ${f} NAME_WE)
  foreach(mode interp compiled types wall)
    funk_test(lib/${n}/${mode} lib run ${f} ${mode} -M lib)
  endforeach()
endforeach()

# reload during playback, through funk_call (bench/reload.c); fails on a misplaced onset
add_test(NAME lib/reload COMMAND reload ${PROJECT_SOURCE_DIR}/lib 4000)
set_tests_properties(lib/reload PROPERTIES LABELS lib TIMEOUT 300)

# funk-midi on a virtual clock, printing its messages (hosts/midi/main.c)
add_test(NAME hosts/midi
  COMMAND sh -c "\"$<TARGET_FILE:funk-midi>\" --no-sleep --script tests/hosts/midi.txt 2>&1 | diff tests/hosts/midi.expected -"
  WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
set_tests_properties(hosts/midi PROPERTIES LABELS hosts TIMEOUT 300)
# ... with --audio: a graph of lib/kernels.fk, rendered on the virtual clock
add_test(NAME hosts/audio
  COMMAND sh -c "\"$<TARGET_FILE:funk-midi>\" --audio --no-sleep --script tests/hosts/audio.txt 2>&1 | diff tests/hosts/audio.expected -"
  WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
set_tests_properties(hosts/audio PROPERTIES LABELS hosts TIMEOUT 300 ENVIRONMENT "XDG_CACHE_HOME=${CMAKE_CURRENT_BINARY_DIR}/cache")

# libfunk.h from a host's C (docs/dev/embedding.md)
foreach(n signals output interrupt memory)
  add_executable(embed_${n} embed/${n}.c)
  target_link_libraries(embed_${n} PRIVATE libfunk)
  if(FUNK_SANITIZE)
    target_compile_options(embed_${n} PRIVATE -fsanitize=${FUNK_SANITIZE} -fno-sanitize-recover=all)
    target_link_options(embed_${n} PRIVATE -fsanitize=${FUNK_SANITIZE})
  endif()
endforeach()
add_test(NAME embed/signals COMMAND embed_signals)
add_test(NAME embed/signals/stress COMMAND embed_signals)
set_tests_properties(embed/signals embed/signals/stress PROPERTIES LABELS embed TIMEOUT 300
  PASS_REGULAR_EXPRESSION "host handler: 3 faults, page 3\nafter funk_main: host handler")
set_tests_properties(embed/signals/stress PROPERTIES ENVIRONMENT FUNK_GCSTRESS=1)
# stdout only: anything funk_output should have collected would show here
add_test(NAME embed/output
  COMMAND sh -c "\"$<TARGET_FILE:embed_output>\" 2>/dev/null | diff tests/embed/output.expected -"
  WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
set_tests_properties(embed/output PROPERTIES LABELS embed TIMEOUT 300)
add_test(NAME embed/interrupt
  COMMAND sh -c "\"$<TARGET_FILE:embed_interrupt>\" 2>/dev/null | diff tests/embed/interrupt.expected -"
  WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
set_tests_properties(embed/interrupt PROPERTIES LABELS embed TIMEOUT 300)
add_test(NAME embed/memory
  COMMAND sh -c "\"$<TARGET_FILE:embed_memory>\" 2>&1 | diff tests/embed/memory.expected -"
  WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
add_test(NAME embed/memory/stress
  COMMAND sh -c "\"$<TARGET_FILE:embed_memory>\" 2>&1 | diff tests/embed/memory.expected -"
  WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
set_tests_properties(embed/memory embed/memory/stress PROPERTIES LABELS embed TIMEOUT 300)
set_tests_properties(embed/memory/stress PROPERTIES ENVIRONMENT FUNK_GCSTRESS=1)

# Benchmarks Game programs (bench/game) at their small n, against the
# Game's output files
# regex-redux and pidigits use PCRE2 and GMP; they run only where found
set(LIB_HINTS /opt/homebrew /usr/local)
find_path(PCRE_INCLUDE pcre2.h HINTS ${LIB_HINTS} PATH_SUFFIXES include)
find_library(PCRE_LIB pcre2-8 HINTS ${LIB_HINTS} PATH_SUFFIXES lib)
find_path(GMP_INCLUDE gmp.h HINTS ${LIB_HINTS} PATH_SUFFIXES include)
find_library(GMP_LIB gmp HINTS ${LIB_HINTS} PATH_SUFFIXES lib)
file(GLOB games RELATIVE ${PROJECT_SOURCE_DIR} CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/bench/game/*.fk)
foreach(f ${games})
  get_filename_component(n ${f} NAME_WE)
  set(flags)
  if(n STREQUAL "regexredux" OR n STREQUAL "pidigits")
    set(lib PCRE)
    if(n STREQUAL "pidigits")
      set(lib GMP)
    endif()
    if(NOT ${lib}_INCLUDE OR NOT ${lib}_LIB)
      message(STATUS "No ${lib}: the ${n} tests are skipped")
      continue()
    endif()
    get_filename_component(libdir ${${lib}_LIB} DIRECTORY)
    set(flags -I bench/game -I ${${lib}_INCLUDE} -L ${libdir}) # bench/game: pcre2_8.h
  endif()
  foreach(mode interp compiled types wall)
    funk_test(game/${n}/${mode} game run ${f} ${mode} ${flags})
  endforeach()
endforeach()

# under constant incremental collection, which checks every cycle's marks
foreach(n gc gcbarrier gcdeep)
  foreach(mode interp compiled)
    funk_test(stress/${n}/${mode} stress run tests/run/${n}.fk stress-${mode})
  endforeach()
endforeach()
funk_test(gcstats gc run tests/run/gc.fk gcstats)

file(GLOB errors RELATIVE ${PROJECT_SOURCE_DIR} CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/tests/errors/*.fk)
foreach(f ${errors})
  get_filename_component(n ${f} NAME_WE)
  funk_test(error/${n} error error ${f})
endforeach()

file(GLOB repls RELATIVE ${PROJECT_SOURCE_DIR} CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/tests/repl/*.in)
foreach(f ${repls})
  get_filename_component(n ${f} NAME_WE)
  funk_test(repl/${n} repl repl ${f})
endforeach()

# A library outside the default search paths, found with -I, -L and -l
add_library(fkt STATIC ffi/fkt.c)
set_target_properties(fkt PROPERTIES POSITION_INDEPENDENT_CODE ON)
set(LINK -I tests/ffi -L $<TARGET_FILE_DIR:fkt> -lfkt)
foreach(n link callback ring)
  foreach(mode interp compiled)
    funk_test(ffi/${n}/${mode} ffi run tests/ffi/${n}.fk ${mode} ${LINK})
  endforeach()
endforeach()
# a C call that has the kernel write its stack while a cycle's snapshot is read-only
foreach(mode interp compiled)
  funk_test(ffi/efault/${mode} "ffi;stress" run tests/ffi/efault.fk stress-${mode} ${LINK})
endforeach()
# exports called from the library's thread while funk collects
foreach(n export control)
  funk_test(ffi/${n}/compiled ffi run tests/ffi/${n}.fk compiled ${LINK})
  funk_test(ffi/${n}/stress-compiled "ffi;stress" run tests/ffi/${n}.fk stress-compiled ${LINK})
endforeach()
# the export's thread against the collector's, under ThreadSanitizer, where the compiler has it
include(CheckCSourceCompiles)
set(CMAKE_REQUIRED_FLAGS -fsanitize=thread)
set(CMAKE_REQUIRED_LINK_OPTIONS -fsanitize=thread)
check_c_source_compiles("int main(void) { return 0; }" HAVE_TSAN)
unset(CMAKE_REQUIRED_FLAGS)
unset(CMAKE_REQUIRED_LINK_OPTIONS)
if(HAVE_TSAN)
  funk_test(ffi/export/tsan-compiled "ffi;stress;tsan" run tests/ffi/export.fk tsan-compiled ${LINK})
else()
  message(STATUS "No -fsanitize=thread: the tsan test is skipped")
endif()
# lib/pattern.fk's scheduler: a ring to an export on the paced audio host
foreach(mode compiled stress-compiled)
  funk_test(ffi/sched/${mode} "ffi;lib" run tests/ffi/sched.fk ${mode} ${LINK} -M lib)
endforeach()
if(HAVE_TSAN)
  funk_test(ffi/sched/tsan-compiled "ffi;lib;stress;tsan" run tests/ffi/sched.fk tsan-compiled ${LINK} -M lib)
endif()
# exports without the runtime, loaded by a C host (docs/dev/audio.md, D)
funk_test(kernels/chain "ffi;kernels" kernels tests/kernels/chain.fk tests/kernels/host.c)
# ... run as a graph by hosts/graph, swapped while an audio thread renders
set(GRAPH kernels tests/kernels/chain.fk tests/kernels/graph.c hosts/graph/graph.c -Ihosts/graph -lpthread -lm)
funk_test(kernels/graph "ffi;kernels" ${GRAPH})
# ... built from funk by lib/graph.fk
set(FUNKGRAPH -M lib -I hosts/graph -L $<TARGET_FILE_DIR:funkgraph> -lfunkgraph)
funk_test(kernels/build "ffi;kernels;lib" kernels tests/kernels/chain.fk tests/kernels/build.fk ${FUNKGRAPH})
funk_test(kernels/dupname "ffi;kernels;lib" error tests/kernels/dupname.fk ${FUNKGRAPH})
# lib/dsp.fk over lib/kernels.fk
funk_test(kernels/ugens "ffi;kernels;lib" kernels lib/kernels.fk tests/kernels/ugens.fk ${FUNKGRAPH})
set(CMAKE_REQUIRED_FLAGS -fsanitize=address)
set(CMAKE_REQUIRED_LINK_OPTIONS -fsanitize=address)
check_c_source_compiles("int main(void) { return 0; }" HAVE_ASAN)
unset(CMAKE_REQUIRED_FLAGS)
unset(CMAKE_REQUIRED_LINK_OPTIONS)
foreach(san address thread)
  if((san STREQUAL address AND HAVE_ASAN) OR (san STREQUAL thread AND HAVE_TSAN))
    funk_test(kernels/graph/${san} "ffi;kernels;stress" ${GRAPH})
    set_tests_properties(kernels/graph/${san} PROPERTIES ENVIRONMENT "FUNK=$<TARGET_FILE:funk>;HOST_CFLAGS=-fsanitize=${san}")
  endif()
endforeach()

foreach(n cberror cbthread)
  funk_test(ffi/${n} ffi error tests/ffi/${n}.fk ${LINK})
endforeach()

include(CheckIncludeFile)
check_include_file(sqlite3.h HAVE_SQLITE3_H)
if(HAVE_SQLITE3_H)
  foreach(mode interp compiled)
    funk_test(ffi/sqlite/${mode} "ffi;sqlite" run tests/ffi/sqlite.fk ${mode})
  endforeach()
else()
  message(STATUS "No sqlite3.h: the sqlite tests are skipped")
endif()

# exit status 0 on success, 1 on a failing program, 2 on a usage error;
# the first line of output, unless -. Arguments are separated by ^.
set(clis
  "0|-|--version"
  "0|-|help^build"
  "0|3|-e^1 + 2"
  "0|[2,4]|eval^map (* 2) [1, 2]"
  "1|-|eval^1 +"
  "0|-|check^tests/run/strings.fk"
  "1|-|check^tests/errors/loc_case.fk"
  "2|-|biuld^x.fk"
  "2|-|run"
  "2|-|build^tests/run/strings.fk^-Ofast"
  "2|-|emit-c^tests/run/strings.fk^-x"
  "1|funk: tests/run/strings.fk: no `foreign export` to compile|emit-c^tests/run/strings.fk^--kernels"
  "1|funk: tests/kernels/alloc.fk: `digits`, a top-level value an export reads, may allocate or fail: the primitive `primShowInt`, at 6:18 in `digits`|emit-c^tests/kernels/alloc.fk^--kernels"
  "2|-|build^tests/run/strings.fk^--kernels"
  "0|/* link: -lm */|emit-c^tests/kernels/chain.fk^--kernels"
  "0|/* link: -lsqlite3 -lm */|emit-c^tests/kernels/libs.fk^--kernels"
  "1|-|run^tests/run/no-such-file.fk")
set(i 0)
foreach(c ${clis})
  string(REPLACE "|" ";" c "${c}")
  string(REPLACE "^" ";" c "${c}")
  list(POP_FRONT c status out)
  list(GET c 0 first)
  math(EXPR i "${i} + 1")
  funk_test(cli/${i}${first} cli cli ${status} ${out} ${c})
endforeach()
