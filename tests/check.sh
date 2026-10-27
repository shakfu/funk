#!/bin/sh
# One test of the suite; CMakeLists.txt lists them for ctest. FUNK is the
# funk binary. Each test has its own directory and foreign-import cache,
# so tests run in parallel. Exit status 0 is a pass.
#
#   check.sh run FILE MODE [FLAGS]  FILE's output, with stdin from FILE.stdin if present,
#                                   must equal FILE.expected. MODE is one of
#                                   interp, compiled: the backend
#                                   stress-interp, stress-compiled: the same under FUNK_GCSTRESS=1
#                                   types: every expression has a type (FK_CHECK_TYPES)
#                                   wall: the C compiles without warnings under -Wall -Wextra
#                                   reuse, fuse: the reuse copies (@reuseK, @ownK) emitted without
#                                     fusion, or the fused loops, equal FILE.reuse or FILE.fuse
#                                   gcstats: FUNK_GCSTATS=1 reports each cycle and a summary
#                                   FLAGS (-I, -L, -l) go to funk run and funk build
#   check.sh error FILE [FLAGS]     funk run FILE fails, and its output contains the text
#                                   after "-- expect: " on FILE's first line
#   check.sh repl FILE              the REPL's output for FILE equals FILE without .in, .expected
#   check.sh cli STATUS OUTPUT ARGS funk ARGS exits with STATUS, and prints OUTPUT first
#                                   unless OUTPUT is -
cd "$(dirname "$0")/.." || exit 1
: "${FUNK:?FUNK must name the funk binary}"
tmp=$(mktemp -d) || exit 1
trap 'rm -rf "$tmp"' EXIT
export XDG_CACHE_HOME="$tmp/cache" # foreign-import glue, compiled afresh

fail() {
  echo "FAIL: $1"
  [ -n "${2:-}" ] && sed 's/^/    /' "$2" | head -40
  exit 1
}
same() { diff "$1" "$2" >"$tmp/diff" 2>&1 || fail "output differs from $1" "$tmp/diff"; }

kind=$1
shift
case $kind in
run)
  f=$1 mode=$2
  shift 2
  base=${f%.fk}
  in=/dev/null
  [ -f "$base.stdin" ] && in=$base.stdin
  build() { "$FUNK" build "$f" -o "$tmp/exe" "$@" >"$tmp/cc" 2>&1 || fail "funk build" "$tmp/cc"; }
  case $mode in
  interp) "$FUNK" run "$f" "$@" <"$in" >"$tmp/out" 2>&1 ;;
  stress-interp) FUNK_GCSTRESS=1 "$FUNK" run "$f" "$@" <"$in" >"$tmp/out" 2>&1 ;;
  compiled) build "$@" && "$tmp/exe" <"$in" >"$tmp/out" 2>&1 ;;
  stress-compiled) build "$@" && FUNK_GCSTRESS=1 "$tmp/exe" <"$in" >"$tmp/out" 2>&1 ;;
  types)
    FK_CHECK_TYPES=1 "$FUNK" emit-c "$f" -o /dev/null 2>"$tmp/types" || fail "funk emit-c" "$tmp/types"
    if grep -q untyped "$tmp/types"; then fail "expressions without a type" "$tmp/types"; fi
    exit 0 ;;
  wall)
    "$FUNK" emit-c "$f" -o "$tmp/w.c" || fail "funk emit-c"
    inc= # FLAGS' -I DIR only: -L is unused with -c, which -Werror would fail
    while [ $# -gt 0 ]; do
      case $1 in -I) inc="$inc -I $2"; shift ;; -I*) inc="$inc $1" ;; esac
      shift
    done
    # overflow.fk recurses forever on purpose
    # shellcheck disable=SC2086
    ${CC:-cc} -O2 -Wall -Wextra -Wno-unused-parameter -Wno-infinite-recursion -Werror $inc -c -o /dev/null "$tmp/w.c" >"$tmp/cc" 2>&1 ||
      fail "warnings in the generated C" "$tmp/cc"
    exit 0 ;;
  reuse)
    FK_NOFUSE=1 "$FUNK" emit-c "$f" -o "$tmp/c" || fail "funk emit-c"
    grep '^static .* w_[0-9]*(.*) { /\* .*@\(reuse\|own\)[0-9]* \*/$' "$tmp/c" | sed 's|.*/\* ||; s| \*/$||' | LC_ALL=C sort >"$tmp/out"
    same "$base.reuse" "$tmp/out"
    exit 0 ;;
  fuse)
    "$FUNK" emit-c "$f" -o "$tmp/c" || fail "funk emit-c"
    # a fused loop's name ends in its id, or in its reuse copies' suffixes
    grep '^static .* w_[0-9]*(.*) { /\* .*/\$f[0-9]*\(@\(reuse\|own\)[0-9]*\)* \*/$' "$tmp/c" | sed 's|.*/\* ||; s|/\$f.*||' | LC_ALL=C sort -u >"$tmp/out"
    same "$base.fuse" "$tmp/out"
    exit 0 ;;
  gcstats)
    build "$@"
    FUNK_GCSTATS=1 "$tmp/exe" <"$in" >/dev/null 2>"$tmp/stats"
    grep -q '^gc 1: start .* ms, [0-9]* steps (max .* ms), finish .* ms, mark .* ms in all, live .* MiB, heap .* MiB$' "$tmp/stats" &&
      grep -q '^gc: [0-9]* cycles, pause max .* ms (start .*, mark step .*, finish .*, sweep step .*), total .* ms, [0-9]* forced; lazy sweep [0-9]* chunks' "$tmp/stats" ||
      fail "FUNK_GCSTATS output" "$tmp/stats"
    exit 0 ;;
  *) fail "unknown mode $mode" ;;
  esac
  same "$base.expected" "$tmp/out" ;;
error)
  f=$1
  shift
  want=$(head -n 1 "$f" | sed 's/^-- expect: //')
  # the subshell reports an abort, as cbthread.fk's, to /dev/null
  if ("$FUNK" run "$f" "$@" </dev/null >"$tmp/out" 2>&1; exit $?) 2>/dev/null; then fail "expected a failure" "$tmp/out"; fi
  grep -qF -- "$want" "$tmp/out" || { echo "want: $want" >>"$tmp/out"; fail "message" "$tmp/out"; } ;;
repl)
  "$FUNK" <"$1" >"$tmp/out" 2>&1
  same "${1%.in}.expected" "$tmp/out" ;;
cli)
  want=$1 out=$2
  shift 2
  "$FUNK" "$@" >"$tmp/out" 2>&1
  got=$?
  [ "$got" -eq "$want" ] || { echo "exit $got, wanted $want" >>"$tmp/out"; fail "funk $*" "$tmp/out"; }
  [ "$out" = - ] || [ "$(head -n 1 "$tmp/out")" = "$out" ] || fail "funk $*: first line, wanted $out" "$tmp/out" ;;
*) fail "unknown kind $kind" ;;
esac
