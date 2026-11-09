#!/bin/sh
# funk-midi --dirt against Tidal's superdirtTarget: tests/tidal/Dirt.hs builds
# Tidal's /dirt/play messages for each case of tests/tidal/dirt.cases through
# Tidal's toOSC; funk-midi plays the same cases on its virtual clock and
# prints the messages it encodes. Their arguments must be the same.
# Arguments: ghc, funk-midi, a scratch directory. Run from the repository root.
ghc="$1" midi="$2" dir="$3"
set -e
mkdir -p "$dir"
containers=$(ghc-pkg field tidal-core depends | grep -o 'containers-[0-9.]*' | head -1)
"$ghc" -O0 -package tidal -package hosc -package "$containers" -outputdir "$dir/o" tests/tidal/Dirt.hs -o "$dir/dirt" >/dev/null
"$dir/dirt" > "$dir/tidal.out"
: > "$dir/funk.out"
tab=$(printf '\t')
while IFS="$tab" read -r name expr; do
  # cycles [0, 4) at 2 s a cycle: the last window starts before 8000 ms
  printf 'pat = %s\n:wait 7890\n' "$expr" > "$dir/case.txt"
  "$midi" --no-sleep --dirt 57120 --script "$dir/case.txt" | sed -n "s|^ *[0-9.]* dirt |$name |p" >> "$dir/funk.out"
done < tests/tidal/dirt.cases
diff "$dir/tidal.out" "$dir/funk.out"
