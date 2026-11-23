#!/bin/sh
# ffi_cache_race.sh FUNK [PROCS [ROUNDS [PROGRAM]]]: PROCS processes run PROGRAM
# at once into one empty FFI cache, ROUNDS times, so that each round all of them
# compile the same foreign imports (ffi.c, ffi_load). Prints each round's exit
# codes and the stderr of any process that failed; exits 1 if any failed.
# Defaults: 8 processes, 10 rounds, tests/run/ffi.fk. Run from the source root.
# A manual tool, not run by ctest: the old shared-source race never failed it.
set -u
funk=${1:?usage: ffi_cache_race.sh FUNK [PROCS [ROUNDS [PROGRAM]]]}
procs=${2:-8}
rounds=${3:-10}
prog=${4:-tests/run/ffi.fk}
work=$(mktemp -d "${TMPDIR:-/tmp}/ffi-race.XXXXXX") || exit 2
trap 'rm -rf "$work"' EXIT
failed=0
r=1
while [ "$r" -le "$rounds" ]; do
  cache="$work/cache$r"
  mkdir -m 700 "$cache"
  i=1
  while [ "$i" -le "$procs" ]; do
    # each waits for the go file, so that all start compiling together
    (while [ ! -e "$work/go$r" ]; do :; done
     XDG_CACHE_HOME="$cache" "$funk" run "$prog" >/dev/null 2>"$work/err$r.$i"
     echo $? >"$work/code$r.$i") &
    i=$((i + 1))
  done
  touch "$work/go$r"
  wait
  codes=""
  i=1
  while [ "$i" -le "$procs" ]; do
    c=$(cat "$work/code$r.$i")
    codes="$codes $c"
    if [ "$c" != 0 ]; then
      failed=$((failed + 1))
      echo "round $r, process $i exited $c:"
      sed 's/^/  /' "$work/err$r.$i" | head -20
    fi
    i=$((i + 1))
  done
  echo "round $r:$codes"
  r=$((r + 1))
done
echo "$failed of $((procs * rounds)) processes failed"
[ "$failed" = 0 ]
