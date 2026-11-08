"""Check that examples/tidal's patches are tests/tidal/ports.fk's.

Usage: examples.py (from the repository root)

Each examples/tidal/cycles-N.fk, or cycles-N-M.fk, defines pat as ports.fk's
entry "pN" or "pN.M", up to whitespace. Every entry has an example, but the
ones silent in Tidal 1.10.2 (examples/tidal/README.md).
"""

import glob
import os
import re
import sys

SILENT = {"p6", "p6.1", "p6.2", "p15"}


def words(s):
    return " ".join(s.split())


def main():
    src = open("tests/tidal/ports.fk").read().split("patches =", 1)[1]
    names = re.findall(r'\("(p[0-9.]+)", \d+, ', src)
    parts = re.split(r'\("p[0-9.]+", \d+, ', src)[1:]
    ports = {n: words(re.sub(r"\)\s*[,\]]?\s*$", "", p.rstrip())) for n, p in zip(names, parts)}
    bad = 0
    seen = set()
    for f in sorted(glob.glob("examples/tidal/cycles-*.fk")):
        m = re.fullmatch(r"cycles-(\d+)(?:-(\d+))?\.fk", os.path.basename(f))
        name = "p%d" % int(m.group(1)) + ("." + m.group(2) if m.group(2) else "")
        seen.add(name)
        pat = words(open(f).read().split("\npat = ", 1)[1])
        if pat != ports.get(name):
            print(f"{f}: pat is not ports.fk's {name}")
            bad += 1
    for name in sorted(set(ports) - SILENT - seen):
        print(f"{name}: no example")
        bad += 1
    print(f"{len(seen)} examples, {bad} differ")
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
