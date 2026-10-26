#!/bin/sh
# Run the funk test suite. Exit status is the number of failures (capped).
#   tests/run/NAME.fk      interpreted and compiled output must equal NAME.expected;
#                          the reuse copies emitted without fusion must equal
#                          NAME.reuse, and the fused loops NAME.fuse, if present
#   examples/NAME.fk       (stdin from NAME.stdin if present); every expression
#                          must have a type (FK_CHECK_TYPES), for the typed backend
#   tests/errors/NAME.fk   must exit non-zero; stderr must contain the text
#                          after "-- expect: " on line 1
#   tests/repl/NAME.in     REPL stdout+stderr must equal NAME.expected
#   tests/ffi/link.fk      foreign imports from a library found with -I, -L and -l
#   tests/ffi/ring.fk      the queued-callback ring: overflow, and messages too large for a slot
#   tests/ffi/export.fk    a foreign export called from the library's thread while funk
#                          collects, compiled, and under FUNK_GCSTRESS
#   tests/ffi/callback.fk  callbacks into funk from that library; cberror.fk and
#                          cbthread.fk must fail with their "-- expect: " text
#   tests/ffi/sqlite.fk    sqlite3, in both backends; skipped without sqlite3.h
#   the command line       exit status 0 on success, 1 on a failing program, 2 on a usage error
#   FUNK_GCSTATS=1         gc.fk reports each collection and a summary
#   FUNK_GCSTRESS=1        gc.fk and gcbarrier.fk, in both backends, under constant
#                          incremental collection, which checks every cycle's marks
cd "$(dirname "$0")/.." || exit 1
FUNK=${FUNK:-./funk}
tmp=$(mktemp -d) || exit 1
trap 'rm -rf "$tmp"' EXIT
export XDG_CACHE_HOME="$tmp/cache" # foreign-import glue, compiled afresh
pass=0
fail=0

ok() { pass=$((pass + 1)); }
bad() { fail=$((fail + 1)); echo "FAIL $1"; [ -n "$2" ] && sed 's/^/    /' "$2" | head -20; }

for f in tests/run/*.fk examples/*.fk; do
  n=$(basename "$f" .fk)
  d=$(dirname "$f")
  in=/dev/null
  [ -f "$d/$n.stdin" ] && in="$d/$n.stdin"
  "$FUNK" "$f" <"$in" >"$tmp/$n.interp" 2>&1
  if diff "$d/$n.expected" "$tmp/$n.interp" >"$tmp/diff" 2>&1; then ok; else bad "$n (interpreter)" "$tmp/diff"; fi
  if "$FUNK" build "$f" -o "$tmp/$n.exe" >"$tmp/cc" 2>&1; then
    "$tmp/$n.exe" <"$in" >"$tmp/$n.compiled" 2>&1
    if diff "$d/$n.expected" "$tmp/$n.compiled" >"$tmp/diff" 2>&1; then ok; else bad "$n (compiled)" "$tmp/diff"; fi
  else
    bad "$n (build)" "$tmp/cc"
  fi
  if FK_CHECK_TYPES=1 "$FUNK" emit-c "$f" -o /dev/null 2>"$tmp/types" && ! grep -q untyped "$tmp/types"; then ok
  else bad "$n (types)" "$tmp/types"; fi
  if [ -f "$d/$n.reuse" ]; then
    FK_NOFUSE=1 "$FUNK" emit-c "$f" -o "$tmp/$n.c" && grep '^static .* w_[0-9]*(.*) { /\* .*@reuse[0-9]* \*/$' "$tmp/$n.c" |
      sed 's|.*/\* ||; s| \*/$||' | sort >"$tmp/$n.reuse"
    if diff "$d/$n.reuse" "$tmp/$n.reuse" >"$tmp/diff" 2>&1; then ok; else bad "$n (reuse)" "$tmp/diff"; fi
  fi
  if [ -f "$d/$n.fuse" ]; then
    "$FUNK" emit-c "$f" -o "$tmp/$n.c" && grep '^static .* w_[0-9]*(.*) { /\* .*/\$f[0-9]' "$tmp/$n.c" |
      sed 's|.*/\* ||; s|/\$f.*||' | sort >"$tmp/$n.fuse"
    if diff "$d/$n.fuse" "$tmp/$n.fuse" >"$tmp/diff" 2>&1; then ok; else bad "$n (fuse)" "$tmp/diff"; fi
  fi
done

for f in tests/errors/*.fk; do
  n=$(basename "$f" .fk)
  want=$(head -n 1 "$f" | sed 's/^-- expect: //')
  if "$FUNK" "$f" </dev/null >"$tmp/out" 2>&1; then
    bad "$n (expected failure, got success)" "$tmp/out"
  elif grep -qF -- "$want" "$tmp/out"; then
    ok
  else
    echo "    want: $want" >>"$tmp/out"
    bad "$n" "$tmp/out"
  fi
done

if cc -fPIC -c tests/ffi/fkt.c -o "$tmp/fkt.o" && ar rcs "$tmp/libfkt.a" "$tmp/fkt.o"; then
  link="-I tests/ffi -L $tmp -lfkt"
  # shellcheck disable=SC2086
  "$FUNK" run tests/ffi/link.fk $link >"$tmp/link.out" 2>&1
  if diff tests/ffi/link.expected "$tmp/link.out" >"$tmp/diff" 2>&1; then ok; else bad "link (interpreter)" "$tmp/diff"; fi
  # shellcheck disable=SC2086
  if "$FUNK" build tests/ffi/link.fk -o "$tmp/link" $link >"$tmp/cc" 2>&1 && "$tmp/link" >"$tmp/link.out" 2>&1 &&
    diff tests/ffi/link.expected "$tmp/link.out" >"$tmp/diff" 2>&1; then ok; else bad "link (compiled)" "$tmp/diff"; fi
  for n in callback ring; do
    # shellcheck disable=SC2086
    "$FUNK" run tests/ffi/$n.fk $link >"$tmp/$n.out" 2>&1
    if diff tests/ffi/$n.expected "$tmp/$n.out" >"$tmp/diff" 2>&1; then ok; else bad "$n (interpreter)" "$tmp/diff"; fi
    # shellcheck disable=SC2086
    if "$FUNK" build tests/ffi/$n.fk -o "$tmp/$n" $link >"$tmp/cc" 2>&1 && "$tmp/$n" >"$tmp/$n.out" 2>&1 &&
      diff tests/ffi/$n.expected "$tmp/$n.out" >"$tmp/diff" 2>&1; then ok; else bad "$n (compiled)" "$tmp/diff"; fi
  done
  # shellcheck disable=SC2086
  if "$FUNK" build tests/ffi/export.fk -o "$tmp/export" $link >"$tmp/cc" 2>&1; then
    "$tmp/export" >"$tmp/export.out" 2>&1
    if diff tests/ffi/export.expected "$tmp/export.out" >"$tmp/diff" 2>&1; then ok; else bad "export" "$tmp/diff"; fi
    FUNK_GCSTRESS=1 "$tmp/export" >"$tmp/export.out" 2>&1
    if diff tests/ffi/export.expected "$tmp/export.out" >"$tmp/diff" 2>&1; then ok; else bad "export (FUNK_GCSTRESS)" "$tmp/diff"; fi
  else
    bad "export (build)" "$tmp/cc"
  fi
  for n in cberror cbthread; do
    want=$(head -n 1 tests/ffi/$n.fk | sed 's/^-- expect: //')
    # shellcheck disable=SC2086 # the subshell reports cbthread's abort to /dev/null
    if ("$FUNK" run tests/ffi/$n.fk $link >"$tmp/out" 2>&1; exit $?) 2>/dev/null; then bad "$n (expected failure, got success)" "$tmp/out"
    elif grep -qF -- "$want" "$tmp/out"; then ok
    else echo "    want: $want" >>"$tmp/out"; bad "$n" "$tmp/out"; fi
  done
else
  bad "link (test library)"
fi

if printf '#include <sqlite3.h>\n' | ${CC:-cc} -E -x c - >/dev/null 2>&1; then
  "$FUNK" tests/ffi/sqlite.fk >"$tmp/sqlite.out" 2>&1
  if diff tests/ffi/sqlite.expected "$tmp/sqlite.out" >"$tmp/diff" 2>&1; then ok; else bad "sqlite (interpreter)" "$tmp/diff"; fi
  if "$FUNK" build tests/ffi/sqlite.fk -o "$tmp/sqlite" >"$tmp/cc" 2>&1 && "$tmp/sqlite" >"$tmp/sqlite.out" 2>&1 &&
    diff tests/ffi/sqlite.expected "$tmp/sqlite.out" >"$tmp/diff" 2>&1; then ok; else bad "sqlite (compiled)" "$tmp/diff"; fi
else
  echo "skip sqlite: no sqlite3.h"
fi

for f in tests/repl/*.in; do
  n=$(basename "$f" .in)
  "$FUNK" <"$f" >"$tmp/$n.repl" 2>&1
  if diff "tests/repl/$n.expected" "$tmp/$n.repl" >"$tmp/diff" 2>&1; then ok; else bad "$n (repl)" "$tmp/diff"; fi
done

for n in gc gcbarrier; do
  FUNK_GCSTRESS=1 "$FUNK" tests/run/$n.fk >"$tmp/$n.stress" 2>&1
  if diff tests/run/$n.expected "$tmp/$n.stress" >"$tmp/diff" 2>&1; then ok; else bad "$n (interpreter, FUNK_GCSTRESS)" "$tmp/diff"; fi
  FUNK_GCSTRESS=1 "$tmp/$n.exe" >"$tmp/$n.stress" 2>&1
  if diff tests/run/$n.expected "$tmp/$n.stress" >"$tmp/diff" 2>&1; then ok; else bad "$n (compiled, FUNK_GCSTRESS)" "$tmp/diff"; fi
done

FUNK_GCSTATS=1 "$tmp/gc.exe" >/dev/null 2>"$tmp/gcstats"
if grep -q '^gc 1: start .* ms, [0-9]* steps (max .* ms), finish .* ms, mark .* ms in all, live .* MiB, heap .* MiB$' "$tmp/gcstats" &&
  grep -q '^gc: [0-9]* cycles, pause max .* ms (start .*, mark step .*, finish .*, sweep step .*), total .* ms, [0-9]* forced; lazy sweep [0-9]* chunks' "$tmp/gcstats"; then ok
else bad "FUNK_GCSTATS" "$tmp/gcstats"; fi

# cli STATUS [OUTPUT] -- ARGS: funk ARGS exits with STATUS, and prints OUTPUT first if given.
cli() {
  want=$1 out=; shift
  if [ "$1" != -- ]; then out=$1; shift; fi
  shift
  "$FUNK" "$@" >"$tmp/cli" 2>&1
  got=$?
  if [ "$got" -ne "$want" ]; then echo "    exit $got, wanted $want" >>"$tmp/cli"; bad "cli: funk $*" "$tmp/cli"
  elif [ -n "$out" ] && [ "$(head -n 1 "$tmp/cli")" != "$out" ]; then bad "cli: funk $*" "$tmp/cli"
  else ok; fi
}
cli 0 -- --version
cli 0 -- help build
cli 0 3 -- -e "1 + 2"
cli 0 "[2,4]" -- eval "map (* 2) [1, 2]"
cli 1 -- eval "1 +"
cli 0 -- check tests/run/strings.fk
cli 1 -- check tests/errors/loc_case.fk
cli 2 -- biuld x.fk
cli 2 -- run
cli 2 -- build tests/run/strings.fk -Ofast
cli 2 -- emit-c tests/run/strings.fk -x
cli 1 -- run tests/run/no-such-file.fk

echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
