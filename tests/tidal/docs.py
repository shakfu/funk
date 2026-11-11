"""Classify funk-midi's messages for each of Tidal's doc examples.

Usage: docs.py CASES TIDAL_OUT FUNK_OUT

Each output line is "name /dirt/play key=type:value ...", a float as its
bits; funk's other lines are "name ! text". Prints one line per case:

  exact     the same messages
  rounded   each message pairs with one within TICKS ticks and VALUE
  tempo     an event's cps sets the tempo (C2, docs/dev/tidal-comparison.md):
            rounded, with delta measured in cycles, on the cycles both
            sides reach, and not at cycle 0, which funk times at the old tempo
  differ    unmatched messages on each side
  rejected  funk's first error

then the largest rounding in ticks. Exits 1 if Tidal printed anything else.
"""

import struct
import sys
from collections import Counter

TICKS = 36
VALUE = 1e-4
CYCLE = 46126080
CPS = 0.5


def parse(line):
    d = {}
    for kv in line.split()[2:]:
        k, v = kv.split("=", 1)
        t, x = v.split(":", 1)
        d[k] = struct.unpack(">f", bytes.fromhex(x))[0] if t == "f" else x
    return d


def load(path, tidal):
    msgs, other = {}, {}
    with open(path) as f:
        for line in f:
            line = line.rstrip("\n")
            name, _, rest = line.partition(" ")
            if rest.startswith("/dirt/play "):
                msgs.setdefault(name, []).append(line)
            elif tidal:
                sys.exit("Tidal: " + line)
            elif rest.startswith("! ") and rest[2:].strip():
                other.setdefault(name, []).append(rest[2:])
    return msgs, other


def close(a, b, tempo):
    """the rounding between a and b in ticks, or None"""
    if a.keys() != b.keys():
        return None
    err = 0.0
    for k in a:
        x, y = a[k], b[k]
        if k == "delta":
            if tempo:
                if a["cycle"] == 0:
                    continue
                x, y = x * CPS, y * b["cps"]
            else:
                x, y = x * CPS, y * CPS
        if k in ("cycle", "delta"):
            e = abs(x - y) * CYCLE
            if e > TICKS:
                return None
            err = max(err, e)
        elif isinstance(x, float):
            if abs(x - y) > VALUE * max(1.0, abs(x)):
                return None
        elif x != y:
            return None
    return err


def pair(ts, fs, tempo):
    """(unmatched Tidal, unmatched funk, largest rounding)"""
    fs = sorted(fs, key=lambda e: e["cycle"])
    used = [False] * len(fs)
    miss, worst = 0, 0.0
    for a in sorted(ts, key=lambda e: e["cycle"]):
        for j, b in enumerate(fs):
            if used[j] or abs(a["cycle"] - b["cycle"]) * CYCLE > TICKS:
                continue
            e = close(a, b, tempo)
            if e is not None:
                used[j] = True
                worst = max(worst, e)
                break
        else:
            miss += 1
    return miss, used.count(False), worst


def main():
    names = [line.split("\t", 1)[0] for line in open(sys.argv[1])]
    tidal, _ = load(sys.argv[2], True)
    funk, errors = load(sys.argv[3], False)
    worst = 0.0
    for n in names:
        tl, fl = tidal.get(n, []), funk.get(n, [])
        if not fl and n in errors:
            print(n, "rejected", errors[n][0])
            continue
        if Counter(tl) == Counter(fl):
            print(n, "exact")
            continue
        ts, fs = [parse(m) for m in tl], [parse(m) for m in fl]
        tempo = any(e.get("cps") != CPS for e in ts + fs)
        if tempo:
            top = min(max((e["cycle"] for e in xs), default=0) for xs in (ts, fs))
            ts = [e for e in ts if e["cycle"] <= top]
            fs = [e for e in fs if e["cycle"] <= top]
        miss, extra, e = pair(ts, fs, tempo)
        if miss or extra:
            print(n, "differ", miss, extra)
        else:
            worst = max(worst, e)
            print(n, "tempo" if tempo else "rounded")
    print("largest rounding:", round(worst), "ticks")


main()
