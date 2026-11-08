#!/bin/sh
# Every graph.h function lib/graph.fk imports is given to funk by both hosts
# (funk_provide). On macOS a missing one still resolves; on Linux the glue
# fails to load, "undefined symbol". Run from the repository root.
status=0
for f in $(sed -n 's/^foreign import ccall "graph.h \([a-z_]*\)".*/\1/p' lib/graph.fk); do
  grep -q "X($f)" projects/funk-midi/main.c || { echo "projects/funk-midi/main.c: GRAPH_FUNCTIONS lacks $f"; status=1; }
  grep -q "PROVIDE($f)" projects/funk-clap/funk_clap.cpp || { echo "projects/funk-clap/funk_clap.cpp: PROVIDE lacks $f"; status=1; }
done
exit $status
