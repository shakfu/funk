#!/bin/sh
# The combinators against Tidal itself: tests/tidal/Oracle.hs, compiled with
# the installed tidal package, and tests/tidal/compare.fk must print the same.
# Arguments: ghc, funk, a scratch directory. Run from the repository root.
ghc="$1" funk="$2" dir="$3"
set -e
mkdir -p "$dir"
# the containers tidal was built with: ghc hides the package when two are installed
containers=$(ghc-pkg field tidal-core depends | grep -o 'containers-[0-9.]*' | head -1)
"$ghc" -O0 -package tidal -package "$containers" -outputdir "$dir/o" tests/tidal/Oracle.hs -o "$dir/oracle" >/dev/null
"$dir/oracle" > "$dir/tidal.out"
"$funk" run tests/tidal/compare.fk -M lib > "$dir/funk.out"
diff "$dir/tidal.out" "$dir/funk.out"
