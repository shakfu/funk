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
cd "$(dirname "$0")/.." || exit 1
FUNK=./funk
tmp=$(mktemp -d) || exit 1
trap 'rm -rf "$tmp"' EXIT
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

for f in tests/repl/*.in; do
  n=$(basename "$f" .in)
  "$FUNK" <"$f" >"$tmp/$n.repl" 2>&1
  if diff "tests/repl/$n.expected" "$tmp/$n.repl" >"$tmp/diff" 2>&1; then ok; else bad "$n (repl)" "$tmp/diff"; fi
done

echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
