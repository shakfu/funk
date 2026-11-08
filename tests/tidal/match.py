"""Match tests/tidal/ports.fk's events to Tidal's, per patch.

Usage: match.py TIDAL_OUT FUNK_OUT

Each line of either file is "name cycle [(start, stop, controls)]", in ticks.
An event matches exactly, or by rounding: start and stop within TICKS ticks,
the same keys and strings, and numbers within VALUE. funk's times are whole
ticks, so a time that is not one rounds (docs/dev/patch-port.md#rounding).
An unmatched event within TICKS of the last cycle's end is counted apart, as
at the edge: its match may round past the cycles queried. Prints one line per
patch, then the largest errors.
"""

import ast
import sys

TICKS = 32
VALUE = 1e-3
CYCLE = 720720


def load(path):
    """name -> (events, the end of the last cycle queried)"""
    out = {}
    with open(path) as f:
        for line in f:
            name, cycle, rest = line.split(" ", 2)
            evs, _ = out.get(name, ([], 0))
            out[name] = (evs + ast.literal_eval(rest), (int(cycle) + 1) * CYCLE)
    return out


def controls(s):
    return dict(kv.split("=", 1) for kv in s.split(";") if kv)


def error(a, b):
    """(ticks, value) apart, or None if a and b cannot match."""
    ca, cb = controls(a[2]), controls(b[2])
    if ca.keys() != cb.keys():
        return None
    dv = 0.0
    for k in ca:
        if ca[k] == cb[k]:
            continue
        if ca[k][0] != "f" or cb[k][0] != "f":
            return None
        dv = max(dv, abs(int(ca[k][1:]) - int(cb[k][1:])) / 1e9)
    return max(abs(a[0] - b[0]), abs(a[1] - b[1])), dv


def match(ts, fs, end):
    rest = sorted(fs)
    exact = 0
    left = []
    for e in sorted(ts):
        if e in rest:
            rest.remove(e)
            exact += 1
        else:
            left.append(e)
    rounded, dt, dv, unmatched, edge = 0, 0, 0.0, 0, 0
    for e in left:
        best = None
        for j, f in enumerate(rest):
            if abs(f[0] - e[0]) > TICKS:
                continue
            err = error(e, f)
            if err and err[0] <= TICKS and err[1] <= VALUE:
                best = (j, err)
                break
        if best is None:
            if abs(e[0] - end) > TICKS:
                unmatched += 1
            else:
                edge += 1
            continue
        j, (t, v) = best
        rest.pop(j)
        rounded += 1
        dt, dv = max(dt, t), max(dv, v)
    for f in rest:
        if abs(f[0] - end) > TICKS:
            unmatched += 1
        else:
            edge += 1
    return exact, rounded, unmatched, edge, dt, dv


def main():
    tidal, funk = load(sys.argv[1]), load(sys.argv[2])
    dt, dv = 0, 0.0
    for name, (ts, end) in tidal.items():
        e, r, u, g, t, v = match(ts, funk.get(name, ([], 0))[0], end)
        dt, dv = max(dt, t), max(dv, v)
        print(f"{name} events {len(ts)} exact {e} rounded {r} unmatched {u} edge {g}")
    print(f"largest error: {dt} ticks, {dv:.6f} in a value")


if __name__ == "__main__":
    main()
