#!/bin/sh
# Tidal's doc examples against funk-midi --dirt (docs/dev/doc-census.md).
# tests/tidal/docs.cases holds the 248 `d1 $` examples of tidal-core 1.10.2's
# and tidal 1.10.3's doc comments that Tidal runs, one per line. ghci plays
# each through tests/tidal/Dirt.hs's out; funk-midi plays it as typed.
# tests/tidal/docs.py classifies each case; the result must be
# tests/tidal/docs.expected.
# Arguments: ghc, funk-midi, python3, a scratch directory. Run from the repository root.
ghc="$1" midi="$2" python="$3" dir="$4"
set -e
mkdir -p "$dir"
containers=$(ghc-pkg field tidal-core depends | grep -o 'containers-[0-9.]*' | head -1)
tab=$(printf '\t')
{
  printf ':set -XOverloadedStrings\nimport Data.Ratio\nimport qualified Data.Map as Map\nimport qualified Data.Fixed\n'
  while IFS="$tab" read -r name expr; do
    printf 'out "%s" (%s)\n' "$name" "$expr"
  done < tests/tidal/docs.cases
} > "$dir/docs.ghci"
"$ghc" --interactive -v0 -package tidal -package hosc -package "$containers" tests/tidal/Dirt.hs < "$dir/docs.ghci" > "$dir/tidal.out" 2>&1
: > "$dir/funk.out"
while IFS="$tab" read -r name expr; do
  # cycles [0, 4) at 2 s a cycle: the last window ends at 8000 ms; not
  # SuperDirt's port, so a running SuperDirt does not play the cases
  printf 'd1 $ %s\n:wait 7999\n' "$expr" > "$dir/case.txt"
  "$midi" --no-sleep --cycle 2000 --dirt 57199 --script "$dir/case.txt" 2>&1 |
    sed -e "s|^ *[0-9.]* dirt |$name |" -e t -e "s|^|$name ! |" >> "$dir/funk.out"
done < tests/tidal/docs.cases
"$python" -I tests/tidal/docs.py tests/tidal/docs.cases "$dir/tidal.out" "$dir/funk.out" | diff tests/tidal/docs.expected -
