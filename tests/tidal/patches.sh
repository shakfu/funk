#!/bin/sh
# Tidal's example patches against their funk port: tests/tidal/Patches.hs,
# compiled with the installed tidal package, and tests/tidal/ports.fk, printed
# by tests/tidal/patches.fk, must match event for event or by rounding
# (tests/tidal/match.py), with the counts in tests/tidal/patches.expected.
# Arguments: ghc, funk, python3, a scratch directory. Run from the repository root.
ghc="$1" funk="$2" python="$3" dir="$4"
set -e
mkdir -p "$dir"
# the containers tidal was built with: ghc hides the package when two are installed
containers=$(ghc-pkg field tidal-core depends | grep -o 'containers-[0-9.]*' | head -1)
"$ghc" -O0 -package tidal -package "$containers" -outputdir "$dir/o" tests/tidal/Patches.hs -o "$dir/patches" >/dev/null
"$dir/patches" > "$dir/tidal.out"
"$funk" run tests/tidal/patches.fk -M lib -M tests/tidal > "$dir/funk.out"
"$python" -I tests/tidal/match.py "$dir/tidal.out" "$dir/funk.out" | diff tests/tidal/patches.expected -
