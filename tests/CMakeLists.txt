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
  foreach(mode reuse fuse report)
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

# graph.fk's imports are given to funk by both hosts, which Linux needs (tests/graph_imports.sh)
add_test(NAME lib/graph_imports COMMAND sh ${PROJECT_SOURCE_DIR}/tests/graph_imports.sh WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
set_tests_properties(lib/graph_imports PROPERTIES LABELS lib)

# reload during playback, through funk_call (bench/reload.c); fails on a misplaced onset
add_test(NAME lib/reload COMMAND reload ${PROJECT_SOURCE_DIR}/lib 4000)
set_tests_properties(lib/reload PROPERTIES LABELS lib TIMEOUT 300)

# funk-midi on a virtual clock, printing its messages (projects/funk-midi)
add_test(NAME hosts/midi
  COMMAND sh -c "\"$<TARGET_FILE:funk-midi>\" --no-sleep --cycle 2000 --script tests/hosts/midi.txt 2>&1 | diff tests/hosts/midi.expected -"
  WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
set_tests_properties(hosts/midi PROPERTIES LABELS hosts TIMEOUT 300)
# ... with --dirt: SuperDirt's bundles, decoded (tests/hosts/dirt.txt)
add_test(NAME hosts/dirt
  COMMAND sh -c "\"$<TARGET_FILE:funk-midi>\" --no-sleep --cycle 2000 --dirt 57120 --script tests/hosts/dirt.txt 2>&1 | diff tests/hosts/dirt.expected -"
  WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
set_tests_properties(hosts/dirt PROPERTIES LABELS hosts TIMEOUT 300)
# ... Tidal's slots, d1 and p "name" (lib/stream.fk), to SuperDirt and to MIDI
add_test(NAME hosts/slots
  COMMAND sh -c "\"$<TARGET_FILE:funk-midi>\" --no-sleep --cycle 2000 --dirt 57120 --script tests/hosts/slots.txt 2>&1 | diff tests/hosts/slots.expected -"
  WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
set_tests_properties(hosts/slots PROPERTIES LABELS hosts TIMEOUT 300)
add_test(NAME hosts/slots-midi
  COMMAND sh -c "\"$<TARGET_FILE:funk-midi>\" --no-sleep --cycle 2000 --script tests/hosts/slots_midi.txt 2>&1 | diff tests/hosts/slots_midi.expected -"
  WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
set_tests_properties(hosts/slots-midi PROPERTIES LABELS hosts TIMEOUT 300)
add_test(NAME hosts/commands
  COMMAND sh -c "\"$<TARGET_FILE:funk-midi>\" --no-sleep --cycle 2000 --dirt 57120 --script tests/hosts/commands.txt 2>&1 | diff tests/hosts/commands.expected -"
  WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
set_tests_properties(hosts/commands PROPERTIES LABELS hosts TIMEOUT 300)
add_test(NAME hosts/tempo
  COMMAND sh -c "\"$<TARGET_FILE:funk-midi>\" --no-sleep --dirt 57120 --script tests/hosts/tempo.txt 2>&1 | diff tests/hosts/tempo.expected -"
  WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
set_tests_properties(hosts/tempo PROPERTIES LABELS hosts TIMEOUT 300)
add_test(NAME hosts/state
  COMMAND sh -c "\"$<TARGET_FILE:funk-midi>\" --no-sleep --cycle 2000 --dirt 57120 --script tests/hosts/state.txt 2>&1 | diff tests/hosts/state.expected -"
  WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
set_tests_properties(hosts/state PROPERTIES LABELS hosts TIMEOUT 300)
add_test(NAME hosts/transitions
  COMMAND sh -c "\"$<TARGET_FILE:funk-midi>\" --no-sleep --cycle 2000 --dirt 57120 --script tests/hosts/transitions.txt 2>&1 | diff tests/hosts/transitions.expected -"
  WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
set_tests_properties(hosts/transitions PROPERTIES LABELS hosts TIMEOUT 300)
# ... both under FUNK_GCSTRESS, whose check of marks read an interpreter
# frame's unset slot as a root (docs/dev/261110-gcstress-chkstk.md)
add_test(NAME hosts/slots-gcstress
  COMMAND sh -c "\"$<TARGET_FILE:funk-midi>\" --no-sleep --cycle 2000 --dirt 57120 --script tests/hosts/slots.txt 2>&1 | diff tests/hosts/slots.expected -"
  WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
add_test(NAME hosts/slots-midi-gcstress
  COMMAND sh -c "\"$<TARGET_FILE:funk-midi>\" --no-sleep --cycle 2000 --script tests/hosts/slots_midi.txt 2>&1 | diff tests/hosts/slots_midi.expected -"
  WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
set_tests_properties(hosts/slots-gcstress hosts/slots-midi-gcstress PROPERTIES LABELS "hosts;stress" TIMEOUT 300 ENVIRONMENT FUNK_GCSTRESS=1)
# ... with --audio: a graph of lib/kernels.fk, rendered on the virtual clock
add_test(NAME hosts/audio
  COMMAND sh -c "\"$<TARGET_FILE:funk-midi>\" --audio --no-sleep --cycle 2000 --script tests/hosts/audio.txt 2>&1 | diff tests/hosts/audio.expected -"
  WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
set_tests_properties(hosts/audio PROPERTIES LABELS hosts TIMEOUT 300 ENVIRONMENT "XDG_CACHE_HOME=${CMAKE_CURRENT_BINARY_DIR}/cache")
# ... pat's notes on voices of the graph
add_test(NAME hosts/voices
  COMMAND sh -c "\"$<TARGET_FILE:funk-midi>\" --audio --voices 2 --no-sleep --cycle 2000 --script tests/hosts/voices.txt 2>&1 | diff tests/hosts/voices.expected -"
  WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
set_tests_properties(hosts/voices PROPERTIES LABELS hosts TIMEOUT 300 ENVIRONMENT "XDG_CACHE_HOME=${CMAKE_CURRENT_BINARY_DIR}/cache")
# ... control patterns as MIDI and on the voices (lib/control.fk)
add_test(NAME hosts/controls
  COMMAND sh -c "\"$<TARGET_FILE:funk-midi>\" --audio --voices 2 --no-sleep --cycle 2000 --script tests/hosts/controls.txt 2>&1 | diff tests/hosts/controls.expected -"
  WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
set_tests_properties(hosts/controls PROPERTIES LABELS hosts TIMEOUT 300 ENVIRONMENT "XDG_CACHE_HOME=${CMAKE_CURRENT_BINARY_DIR}/cache")
# ... d.kit's samples on m.kit's notes, by name, from tests/hosts/samples
add_test(NAME hosts/kit
  COMMAND sh -c "\"$<TARGET_FILE:funk-midi>\" --audio --no-sleep --cycle 2000 --script tests/hosts/kit.txt 2>&1 | diff tests/hosts/kit.expected -"
  WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
set_tests_properties(hosts/kit PROPERTIES LABELS hosts TIMEOUT 300 ENVIRONMENT "XDG_CACHE_HOME=${CMAKE_CURRENT_BINARY_DIR}/cache")
# the CLAP plugin in a headless host: evaluation, clips, Stop, state, Live
# (projects/clap/test_clap_host.cpp; cases in tests/hosts/clap.txt)
if(TARGET funk_midi_clap)
  add_test(NAME hosts/clap_units COMMAND test_clap_units)
  add_test(NAME hosts/clap COMMAND test_clap_host $<TARGET_FILE:funk_midi_clap>
    ${CMAKE_CURRENT_SOURCE_DIR}/hosts/clap.txt 57291)
  set_tests_properties(hosts/clap_units PROPERTIES LABELS "hosts;clap")
  # serial: its real-time checks need the evaluator thread to run within the 50 ms
  # lookahead, which a loaded machine misses: under ctest -j 10, a note 3 ms late
  set_tests_properties(hosts/clap PROPERTIES LABELS "hosts;clap" TIMEOUT 120 RUN_SERIAL TRUE
    ENVIRONMENT "FUNK_MIDI_CLAP_UDP_PORT=57291;XDG_CACHE_HOME=${CMAKE_CURRENT_BINARY_DIR}/cache")
  # simulator mode: a script's steps in turn, logged (projects/clap/simulator.hpp)
  set(SIM_LOG ${CMAKE_CURRENT_BINARY_DIR}/sim.log)
  add_test(NAME hosts/clap/sim COMMAND test_clap_host $<TARGET_FILE:funk_midi_clap> --sim ${SIM_LOG})
  set_tests_properties(hosts/clap/sim PROPERTIES LABELS "hosts;clap" TIMEOUT 60 RUN_SERIAL TRUE
    ENVIRONMENT "FUNK_MIDI_CLAP_SIM=${CMAKE_CURRENT_SOURCE_DIR}/hosts/sim.fk;FUNK_MIDI_CLAP_SIM_LOG=${SIM_LOG};FUNK_MIDI_CLAP_SIM_INTERVAL=0.5;XDG_CACHE_HOME=${CMAKE_CURRENT_BINARY_DIR}/cache")
  # the editor's syntax colours: drawn where ImGui draws its text, headless
  add_test(NAME hosts/clap/colours COMMAND test_editor_colours)
  set_tests_properties(hosts/clap/colours PROPERTIES LABELS "hosts;clap")
  # the editor's syntax colours, where projects/tree-sitter-funk's parser is generated
  if(TARGET test_funk_highlight)
    add_test(NAME hosts/clap/highlight COMMAND test_funk_highlight)
    set_tests_properties(hosts/clap/highlight PROPERTIES LABELS "hosts;clap")
  endif()
  # graph.fk's glue is linked in: with no C compiler, audio still plays and no glue is compiled
  set(CACHE_DIR ${CMAKE_CURRENT_BINARY_DIR}/clap-cache)
  # the host's output is shown only if it fails
  add_test(NAME hosts/clap/nocc COMMAND sh -c "rm -rf \"$0\" && { \"$1\" \"$2\" \"$3\" 57292 >\"$0.log\" 2>&1 || { cat \"$0.log\"; exit 1; }; } && ls \"$0\"/funk/ffi 2>/dev/null | wc -l"
    ${CACHE_DIR} $<TARGET_FILE:test_clap_host> $<TARGET_FILE:funk_midi_clap> ${CMAKE_CURRENT_SOURCE_DIR}/hosts/clap.txt)
  set_tests_properties(hosts/clap/nocc PROPERTIES LABELS "hosts;clap" TIMEOUT 240 RUN_SERIAL TRUE
    ENVIRONMENT "FUNK_MIDI_CLAP_UDP_PORT=57292;CC=/nonexistent/cc;XDG_CACHE_HOME=${CACHE_DIR}" PASS_REGULAR_EXPRESSION "^ *0\n$")
endif()

# libfunk.h from a host's C (docs/dev/embedding.md)
# funk_glue: an import's glue linked in, so no C compiler is run; without it, $CC is
set(LINKED_GLUE ${CMAKE_CURRENT_BINARY_DIR}/embed_glue.c)
add_custom_command(OUTPUT ${LINKED_GLUE}
  COMMAND funk_tool emit-c --glue ${CMAKE_CURRENT_SOURCE_DIR}/embed/linked.fk -o ${LINKED_GLUE}
  DEPENDS funk_tool ${CMAKE_CURRENT_SOURCE_DIR}/embed/linked.fk)
add_executable(embed_glue embed/glue.c ${LINKED_GLUE})
target_include_directories(embed_glue PRIVATE embed)
target_link_libraries(embed_glue PRIVATE libfunk)
add_test(NAME embed/glue COMMAND embed_glue ${CMAKE_CURRENT_SOURCE_DIR}/embed)
add_test(NAME embed/glue/none COMMAND embed_glue ${CMAKE_CURRENT_SOURCE_DIR}/embed --no-glue)
set_tests_properties(embed/glue PROPERTIES LABELS "embed;ffi" PASS_REGULAR_EXPRESSION "^42\n$"
  ENVIRONMENT "CC=/nonexistent/cc;XDG_CACHE_HOME=${CMAKE_CURRENT_BINARY_DIR}/glue-cache")
set_tests_properties(embed/glue/none PROPERTIES LABELS "embed;ffi" PASS_REGULAR_EXPRESSION "failed to compile with /nonexistent/cc"
  ENVIRONMENT "CC=/nonexistent/cc;XDG_CACHE_HOME=${CMAKE_CURRENT_BINARY_DIR}/glue-cache")
# two plugins, each with libfunk inside and only plugin_run exported, loaded RTLD_LOCAL
foreach(p a b)
  add_library(embed_plugin_${p} MODULE embed/plugin.c)
  target_link_libraries(embed_plugin_${p} PRIVATE libfunk)
  if(APPLE)
    target_link_options(embed_plugin_${p} PRIVATE -Wl,-exported_symbol,_plugin_run)
  else()
    target_link_options(embed_plugin_${p} PRIVATE ${FUNK_EXCLUDE_LIBS})
  endif()
endforeach()
# ... and one exporting what libfunk exports
add_library(embed_exports MODULE embed/plugin.c)
target_link_libraries(embed_exports PRIVATE libfunk)
funk_test(embed/exports embed exports $<TARGET_FILE:embed_exports>)
add_executable(embed_plugins embed/plugins.c)
target_link_libraries(embed_plugins PRIVATE ${CMAKE_DL_LIBS})
if(FUNK_SANITIZE) # the modules carry the sanitizer's runtime: macOS wants it in the host from the start
  target_compile_options(embed_plugins PRIVATE -fsanitize=${FUNK_SANITIZE})
  target_link_options(embed_plugins PRIVATE -fsanitize=${FUNK_SANITIZE})
endif()
funk_test(embed/plugin "embed;ffi" plugins $<TARGET_FILE:embed_plugins> $<TARGET_FILE:embed_plugin_a> $<TARGET_FILE:embed_plugin_b>
  tests/embed/plugin.fk -Itests/ffi -Itests/embed -L$<TARGET_FILE_DIR:fkt> -lfkt)
# installed, then found by a host's CMake
set(PKG ${CMAKE_CURRENT_BINARY_DIR}/package)
add_test(NAME embed/package COMMAND sh -c "rm -rf \"$0\" && \"$1\" --install \"$2\" --prefix \"$0/inst\" >/dev/null && \"$1\" -S \"$3\" -B \"$0/b\" -DCMAKE_PREFIX_PATH=\"$0/inst\" -DFUNK_VERSION=$4 >/dev/null && \"$1\" --build \"$0/b\" >/dev/null && \"$0/b/host\""
  ${PKG} ${CMAKE_COMMAND} ${PROJECT_BINARY_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/embed/package ${PROJECT_VERSION})
set_tests_properties(embed/package PROPERTIES LABELS embed TIMEOUT 300 PASS_REGULAR_EXPRESSION "^55\n$")
foreach(n signals output interrupt memory overflow session sessions compile_limit kernels)
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
  PASS_REGULAR_EXPRESSION "host handler: 3 faults, page 3\nafter funk_free: host handler")
set_tests_properties(embed/signals/stress PROPERTIES ENVIRONMENT FUNK_GCSTRESS=1)
# stdout only: anything funk_output should have collected would show here
add_test(NAME embed/output
  COMMAND sh -c "\"$<TARGET_FILE:embed_output>\" 2>/dev/null | diff tests/embed/output.expected -"
  WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
set_tests_properties(embed/output PROPERTIES LABELS embed TIMEOUT 300)
add_test(NAME embed/session
  COMMAND sh -c "\"$<TARGET_FILE:embed_session>\" | diff tests/embed/session.expected -"
  WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
set_tests_properties(embed/session PROPERTIES LABELS embed TIMEOUT 300)
add_test(NAME embed/kernels
  COMMAND sh -c "\"$<TARGET_FILE:embed_kernels>\" | diff tests/embed/kernels.expected -"
  WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
set_tests_properties(embed/kernels PROPERTIES LABELS "embed;kernels" TIMEOUT 300 ENVIRONMENT "XDG_CACHE_HOME=${CMAKE_CURRENT_BINARY_DIR}/kernels-cache")
add_test(NAME embed/sessions
  COMMAND sh -c "\"$<TARGET_FILE:embed_sessions>\" | diff tests/embed/sessions.expected -"
  WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
add_test(NAME embed/sessions/stress
  COMMAND sh -c "\"$<TARGET_FILE:embed_sessions>\" | diff tests/embed/sessions.expected -"
  WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
set_tests_properties(embed/sessions embed/sessions/stress PROPERTIES LABELS embed TIMEOUT 300)
set_tests_properties(embed/sessions/stress PROPERTIES ENVIRONMENT FUNK_GCSTRESS=1 LABELS "embed;stress")
# libfunk's writable variables against an allow-list; its sources again at -O0,
# where clang merges no globals into one symbol
set(FUNK_SRC ${PROJECT_SOURCE_DIR}/projects/funk/src)
add_library(libfunk_globals OBJECT ${FUNK_SRC}/driver.c ${FUNK_SRC}/util.c ${FUNK_SRC}/lex.c ${FUNK_SRC}/parse.c
  ${FUNK_SRC}/check.c ${FUNK_SRC}/elab.c ${FUNK_SRC}/fuse.c ${FUNK_SRC}/resolve.c ${FUNK_SRC}/qualify.c ${FUNK_SRC}/interp.c
  ${FUNK_SRC}/cgen.c ${FUNK_SRC}/ffi.c ${FUNK_SRC}/rt.c)
target_compile_definitions(libfunk_globals PRIVATE FUNK_VERSION="${PROJECT_VERSION}" FK_SESSIONS)
target_compile_options(libfunk_globals PRIVATE -O0)
add_test(NAME embed/globals
  COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/embed/globals.sh ${CMAKE_CURRENT_SOURCE_DIR}/embed/globals.allow $<TARGET_OBJECTS:libfunk_globals>
  COMMAND_EXPAND_LISTS)
set_tests_properties(embed/globals PROPERTIES LABELS embed TIMEOUT 60)
add_test(NAME embed/compile_limit
  COMMAND sh -c "\"$<TARGET_FILE:embed_compile_limit>\" 2>/dev/null")
set_tests_properties(embed/compile_limit PROPERTIES LABELS embed TIMEOUT 120 PASS_REGULAR_EXPRESSION "^ok\n$")
add_test(NAME embed/interrupt
  COMMAND sh -c "\"$<TARGET_FILE:embed_interrupt>\" 2>/dev/null | diff tests/embed/interrupt.expected -"
  WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
set_tests_properties(embed/interrupt PROPERTIES LABELS embed TIMEOUT 300)
foreach(t overflow overflow/stress)
  add_test(NAME embed/${t}
    COMMAND sh -c "\"$<TARGET_FILE:embed_overflow>\" tests/embed/overflow.fk 2>&1 | diff tests/embed/overflow.expected -"
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
endforeach()
set_tests_properties(embed/overflow embed/overflow/stress PROPERTIES LABELS "embed;ffi" TIMEOUT 300)
set_tests_properties(embed/overflow/stress PROPERTIES ENVIRONMENT FUNK_GCSTRESS=1)
if(NOT FUNK_SANITIZE) # sanitizers define malloc themselves
  add_executable(embed_overflow_malloc embed/overflow.c)
  target_compile_definitions(embed_overflow_malloc PRIVATE HOST_MALLOC)
  target_link_libraries(embed_overflow_malloc PRIVATE libfunk)
  add_test(NAME embed/overflow/malloc
    COMMAND sh -c "\"$<TARGET_FILE:embed_overflow_malloc>\" tests/embed/overflow.fk 2>&1"
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
  set_tests_properties(embed/overflow/malloc PROPERTIES LABELS "embed;ffi" TIMEOUT 300
    PASS_REGULAR_EXPRESSION "hook: stack overflow, ctx set\nruntime error: stack overflow\n$")
endif()
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
# ... run as a graph by projects/funkgraph, swapped while an audio thread renders
set(GRAPH kernels tests/kernels/chain.fk tests/kernels/graph.c projects/funkgraph/graph.c projects/funkgraph/voices.c projects/funkgraph/table.c -Iprojects/funkgraph -lpthread -lm)
funk_test(kernels/graph "ffi;kernels" ${GRAPH})
set(TABLES kernels lib/kernels.fk tests/kernels/tables.c projects/funkgraph/graph.c projects/funkgraph/voices.c projects/funkgraph/table.c -Iprojects/funkgraph -lpthread -lm)
funk_test(kernels/tables "ffi;kernels" ${TABLES})
funk_test(kernels/zita "ffi;kernels" kernels lib/kernels.fk tests/kernels/zita.c projects/funkgraph/graph.c projects/funkgraph/voices.c projects/funkgraph/table.c -Iprojects/funkgraph -Itests/kernels -lpthread -lm)
# ... built from funk by lib/graph.fk
set(FUNKGRAPH -M lib -I projects/funkgraph -L $<TARGET_FILE_DIR:funkgraph> -lfunkgraph)
funk_test(kernels/build "ffi;kernels;lib" kernels tests/kernels/chain.fk tests/kernels/build.fk ${FUNKGRAPH})
funk_test(kernels/dupname "ffi;kernels;lib" error tests/kernels/dupname.fk ${FUNKGRAPH})
funk_test(kernels/widths "ffi;kernels;lib" error tests/kernels/widths.fk ${FUNKGRAPH})
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
    funk_test(kernels/tables/${san} "ffi;kernels;stress" ${TABLES})
    set_tests_properties(kernels/tables/${san} PROPERTIES ENVIRONMENT "FUNK=$<TARGET_FILE:funk>;HOST_CFLAGS=-fsanitize=${san}")
  endif()
endforeach()

foreach(n cberror cbthread)
  funk_test(ffi/${n} ffi error tests/ffi/${n}.fk ${LINK})
endforeach()

# Tidal's OSC /ctrl and MIDI CC into a host's controls (projects/funkgraph/controls.h)
add_executable(test_controls kernels/controls.c ${PROJECT_SOURCE_DIR}/projects/funkgraph/controls.c)
target_include_directories(test_controls PRIVATE ${PROJECT_SOURCE_DIR}/projects/funkgraph)
add_test(NAME kernels/controls COMMAND sh -c "\"$<TARGET_FILE:test_controls>\" | diff tests/kernels/controls.expected -" WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
set_tests_properties(kernels/controls PROPERTIES LABELS kernels)

# examples/tidal's patches are tests/tidal/ports.fk's (tests/tidal/examples.py)
find_program(PYTHON3 python3)
if(PYTHON3)
  add_test(NAME lib/tidal-examples COMMAND ${PYTHON3} -I tests/tidal/examples.py WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
  # funk-midi --dirt's handshake, against a stand-in for SuperDirt, in real time
  add_test(NAME hosts/dirt-handshake COMMAND ${PYTHON3} -I tests/hosts/dirt_handshake.py $<TARGET_FILE:funk-midi> WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
  set_tests_properties(hosts/dirt-handshake PROPERTIES LABELS hosts TIMEOUT 60)
  set_tests_properties(lib/tidal-examples PROPERTIES LABELS lib)
else()
  message(STATUS "No python3: lib/tidal-examples and lib/tidal-patches are skipped")
endif()

# the combinators against Tidal, where ghc has the tidal package (tests/tidal/compare.sh)
find_program(GHC ghc)
find_program(GHC_PKG ghc-pkg)
if(GHC AND GHC_PKG)
  execute_process(COMMAND ${GHC_PKG} field tidal-core version RESULT_VARIABLE TIDAL_MISSING OUTPUT_QUIET ERROR_QUIET)
endif()
if(GHC AND GHC_PKG AND TIDAL_MISSING EQUAL 0)
  add_test(NAME lib/tidal COMMAND sh tests/tidal/compare.sh ${GHC} $<TARGET_FILE:funk> ${CMAKE_CURRENT_BINARY_DIR}/tidal WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
  set_tests_properties(lib/tidal PROPERTIES LABELS lib TIMEOUT 600)
  # Tidal's example patches against their port (tests/tidal/patches.sh)
  if(PYTHON3)
    add_test(NAME lib/tidal-patches COMMAND sh tests/tidal/patches.sh ${GHC} $<TARGET_FILE:funk> ${PYTHON3} ${CMAKE_CURRENT_BINARY_DIR}/tidal-patches WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
    set_tests_properties(lib/tidal-patches PROPERTIES LABELS lib TIMEOUT 600)
    # Tidal's doc examples through funk-midi --dirt (tests/tidal/docs.sh)
    add_test(NAME lib/tidal-docs COMMAND sh tests/tidal/docs.sh ${GHC} $<TARGET_FILE:funk-midi> ${PYTHON3} ${CMAKE_CURRENT_BINARY_DIR}/tidal-docs WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
    set_tests_properties(lib/tidal-docs PROPERTIES LABELS lib TIMEOUT 600)
  endif()
  # funk-midi --dirt's messages against Tidal's toOSC (tests/tidal/dirt.sh)
  add_test(NAME lib/tidal-dirt COMMAND sh tests/tidal/dirt.sh ${GHC} $<TARGET_FILE:funk-midi> ${CMAKE_CURRENT_BINARY_DIR}/tidal-dirt WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
  set_tests_properties(lib/tidal-dirt PROPERTIES LABELS lib TIMEOUT 600)
else()
  message(STATUS "No ghc with the tidal package: lib/tidal is skipped")
endif()

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

funk_test(cli/compile_limit cli cli 1 "<prelude>:0:0: error: the compiler exceeded its memory limit (FK_COMPILE_LIMIT, default 2 GiB)" check tests/run/ho_chain.fk)
set_property(TEST cli/compile_limit APPEND PROPERTY ENVIRONMENT FK_COMPILE_LIMIT=1000)

# Under AddressSanitizer (FUNK_SANITIZE=address), what ASan itself changes:
# - run/overflow/interp: ASan prints a warning when a longjmp leaves funk's large stack
#   (rt.c, fk_unwinding), into the output the test compares;
# - embed/memory: its limits are of resident memory, which ASan's shadow memory exceeds;
# - embed/sessions: freed memory waits in ASan's quarantine, so it measures without one.
# LeakSanitizer is off: funk frees a session's memory, not the process's at exit.
if(FUNK_SANITIZE MATCHES "address")
  # embed/overflow: ASan defines malloc, in the test's own image where it links statically, so nothing recovers there
  set_tests_properties(embed/overflow embed/overflow/stress PROPERTIES DISABLED TRUE)
  set_tests_properties(run/overflow/interp embed/memory embed/memory/stress PROPERTIES DISABLED TRUE)
  set_tests_properties(embed/sessions PROPERTIES ENVIRONMENT "ASAN_OPTIONS=detect_leaks=0:quarantine_size_mb=0")
  set_tests_properties(embed/sessions/stress PROPERTIES ENVIRONMENT "ASAN_OPTIONS=detect_leaks=0:quarantine_size_mb=0;FUNK_GCSTRESS=1")
endif()

# Under ThreadSanitizer (FUNK_SANITIZE=thread), what TSan itself changes:
# - deep recursion: TSan's per-thread shadow call stack is a fixed size, and its runtime
#   faults past it (in __tsan::TraceSwitchPartImpl) before funk's stack overflows; at
#   about 30,000 levels in the interpreter on an M1;
# - embed/memory: its limits are of resident memory, which TSan's shadow memory exceeds.
if(FUNK_SANITIZE MATCHES "thread")
  set_tests_properties(run/gc/interp run/gcdeep/interp run/overflow/interp run/reuse/interp run/stress/interp
    run/trmc/interp stress/gc/interp stress/gcdeep/interp error/stack_overflow ffi/efault/interp
    embed/interrupt embed/overflow embed/overflow/stress embed/memory embed/memory/stress PROPERTIES DISABLED TRUE)
endif()
