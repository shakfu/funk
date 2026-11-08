#!/bin/sh
# Parses every .fk file in the repository but three in tests/errors whose
# syntax is their test: layout.fk's layout, and named_default_atom.fk's and
# named_position.fk's named arguments, a default of more than one atom and
# `.{` after an application, which the grammar rejects as funk does. Lists
# the files with ERROR or MISSING nodes and
# fails if there are any. Run from anywhere: sh projects/tree-sitter-funk/check.sh
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
list=$(mktemp)
out=$(mktemp)
trap 'rm -f "$list" "$out"' EXIT
cd "$root" || exit 1
find . -name '*.fk' -not -path './build/*' -not -path './thirdparty/*' -not -path './tests/errors/layout.fk' -not -path './tests/errors/named_default_atom.fk' -not -path './tests/errors/named_position.fk' | sort >"$list"
tree-sitter parse -p "$here" --paths "$list" -q -s >"$out" 2>&1
grep -E '\((ERROR|MISSING)' "$out" | cut -f1 | sed 's/ *$//'
grep "Total parses" "$out"
! grep -qE '\((ERROR|MISSING)' "$out"
