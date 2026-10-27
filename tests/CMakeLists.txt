# The test suite, one ctest test per check; tests/check.sh runs each and
# documents what they check. `make test` runs them in parallel; one by
# name: ctest --test-dir build -R gcdeep. Labels: run, example, error, repl,
# ffi, stress, cli.

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
  foreach(mode interp compiled types)
    funk_test(${label}/${n}/${mode} ${label} run ${f} ${mode})
  endforeach()
  foreach(mode reuse fuse)
    if(EXISTS ${PROJECT_SOURCE_DIR}/${d}/${n}.${mode})
      funk_test(${label}/${n}/${mode} ${label} run ${f} ${mode})
    endif()
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
