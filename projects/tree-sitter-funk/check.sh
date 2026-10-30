#!/bin/sh
# Parses every .fk file in the repository but tests/errors/layout.fk, whose
# layout error is its test; lists the files with ERROR or MISSING nodes and
# fails if there are any. Run from anywhere: sh projects/tree-sitter-funk/check.sh
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
list=$(mktemp)
out=$(mktemp)
trap 'rm -f "$list" "$out"' EXIT
cd "$root" || exit 1
find . -name '*.fk' -not -path './build/*' -not -path './thirdparty/*' -not -path './tests/errors/layout.fk' | sort >"$list"
tree-sitter parse -p "$here" --paths "$list" -q -s >"$out" 2>&1
grep -E '\((ERROR|MISSING)' "$out" | cut -f1 | sed 's/ *$//'
grep "Total parses" "$out"
! grep -qE '\((ERROR|MISSING)' "$out"
