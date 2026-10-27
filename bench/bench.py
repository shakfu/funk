#!/usr/bin/env python3
"""Benchmark funk (interpreted and compiled) against equivalent C.

  bench.py [--suite core|game] run [-r N] [--only a,b] [--backends c,compiled,interp] [--note TEXT]
      Build and time every benchmark; append one row per benchmark and
      backend to bench/results.jsonl (game: bench/game-results.jsonl).
  bench.py [--suite core|game] report [--baseline COMMIT] [--threshold 0.10] [--host HOST]
      Compare the latest run on this host with the previous run, or with
      the latest run of COMMIT. Exits 1 if any benchmark regressed.
  bench.py [--suite core|game] history NAME [--backend compiled] [--host HOST]
      Print the timing of one benchmark across all recorded runs.

The core suite (bench/progs) pairs funk programs with C written to match.
The game suite (bench/game) is programs from the Benchmarks Game, against
its single-threaded C; it is compiled only by default, at its sizes. See
bench/README.md for the benchmarks and the row format.
"""

import argparse
import datetime
import hashlib
import json
import math
import os
import platform
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PROGS = os.path.join(ROOT, "bench", "progs")
RESULTS = os.path.join(ROOT, "bench", "results.jsonl")
BUILD = os.path.join(ROOT, "build", "bench")
FUNK = os.environ.get("FUNK", os.path.join(ROOT, "funk"))
BACKENDS = ["c", "compiled", "interp"]

# Input size per benchmark, written to stdin, and given to the C version
# as argv[1] too. Changing a size makes old rows incomparable; report only
# compares rows with equal n.
CORE_SIZES = {
    "fib": 34,
    "loop": 40000000,
    "lazywhere": 20000000,
    "floats": 10000000,
    "lists": 250000,
    "pipeline": 300000,
    "sort": 300000,
    "queens": 11,
    "trees": 16,
    "closures": 4000000,
    "strings": 300000,
    "records": 30000,
    "bst": 400000,
    "wordfreq": 300000,
    "printing": 1000000,
}

# C takes 0.4-2.8 s at each on an M1, below the Game's own sizes.
GAME_SIZES = {
    "nbody": 10000000,
    "spectralnorm": 3000,
    "fannkuchredux": 11,
    "binarytrees": 18,
    "fasta": 10000000,
    "mandelbrot": 4000,
    "knucleotide": 2500000, # these two read fasta's output for n
    "revcomp": 10000000,
    "regexredux": 1000000,
    "pidigits": 10000,
}
GAME_INPUTS = {"knucleotide", "revcomp", "regexredux"}
# C libraries the Game's programs use, as (header, library), and where to look
GAME_LIBS = {"regexredux": ("pcre2.h", "pcre2-8"), "pidigits": ("gmp.h", "gmp")}
LIB_PREFIXES = ["/opt/homebrew", "/usr/local", "/usr"]


def lib_flags(bench):
    """-I and -L for bench's C library, or [] if it needs none."""
    if SUITE != "game" or bench not in GAME_LIBS:
        return []
    header, _ = GAME_LIBS[bench]
    for p in LIB_PREFIXES:
        if os.path.exists(os.path.join(p, "include", header)):  # -I PROGS: game/pcre2_8.h
            return ["-I", PROGS, "-I", os.path.join(p, "include"), "-L", os.path.join(p, "lib")]
    raise RuntimeError(f"{header} not found under {', '.join(LIB_PREFIXES)}")

SUITES = {
    "core": (PROGS, CORE_SIZES, RESULTS, BACKENDS),
    "game": (os.path.join(ROOT, "bench", "game"), GAME_SIZES, os.path.join(ROOT, "bench", "game-results.jsonl"), ["c", "compiled"]),
}
SIZES = CORE_SIZES


def sh(*cmd):
    return subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True).stdout.strip()


def cpu_name():
    if platform.system() == "Darwin":
        return sh("sysctl", "-n", "machdep.cpu.brand_string")
    try:
        with open("/proc/cpuinfo") as f:
            for line in f:
                if line.startswith("model name"):
                    return line.split(":", 1)[1].strip()
    except OSError:
        pass
    return platform.processor()


def run_meta(note):
    cc = os.environ.get("CC") or "cc"
    dirty = [l for l in sh("git", "status", "--porcelain").splitlines() if not l.endswith("bench/results.jsonl")]
    now = datetime.datetime.now(datetime.timezone.utc).isoformat(timespec="seconds")
    return {
        "run": now,
        "commit": sh("git", "rev-parse", "HEAD"),
        "dirty": bool(dirty),
        "host": platform.node(),
        "machine": f"{platform.system()} {platform.machine()} {cpu_name()}",
        "cc": (sh(*cc.split(), "--version").splitlines() or [cc])[0],
        "note": note,
    }


def measure(cmd, stdin_text):
    """Run cmd once; return (stdout, wall s, user+sys s, max RSS KiB)."""
    with tempfile.TemporaryFile() as out, tempfile.TemporaryFile() as err:
        t0 = time.perf_counter()
        p = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=out, stderr=err)
        p.stdin.write(stdin_text.encode("latin-1"))
        p.stdin.close()
        _, status, ru = os.wait4(p.pid, 0)  # per-child rusage, unlike RUSAGE_CHILDREN
        wall = time.perf_counter() - t0
        p.returncode = os.waitstatus_to_exitcode(status)
        out.seek(0)
        err.seek(0)
        if p.returncode != 0:
            raise RuntimeError(f"{' '.join(cmd)} exited {p.returncode}: {err.read().decode().strip()}")
        rss = ru.ru_maxrss // 1024 if platform.system() == "Darwin" else ru.ru_maxrss  # bytes vs KiB
        return out.read().decode("latin-1"), wall, ru.ru_utime + ru.ru_stime, rss  # bytes as is: mandelbrot writes a binary PBM


def build(bench, backend):
    """Return (command to run, build seconds or None)."""
    os.makedirs(BUILD, exist_ok=True)
    exe = os.path.join(BUILD, f"{SUITE}_{bench}_{backend}")
    flags = lib_flags(bench)
    if backend == "interp":
        return [FUNK, "run", os.path.join(PROGS, bench + ".fk")] + flags, None
    if backend == "compiled":
        cmd = [FUNK, "build", os.path.join(PROGS, bench + ".fk"), "-o", exe] + flags
    else:
        cc = (os.environ.get("CC") or "cc").split()
        libs = ["-l" + GAME_LIBS[bench][1]] if bench in GAME_LIBS and SUITE == "game" else []
        cmd = cc + ["-O2", "-I", PROGS] + flags + ["-o", exe, os.path.join(PROGS, bench + ".c")] + libs + ["-lm"]  # -I: game/khash.h
    t0 = time.perf_counter()
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError(f"build failed: {' '.join(cmd)}\n{r.stdout}{r.stderr}")
    return [exe], time.perf_counter() - t0


def stdin_for(bench, n):
    """What bench reads: n, or for the game's readers, fasta's output for n."""
    if SUITE != "game" or bench not in GAME_INPUTS:
        return f"{n}\n"
    path = os.path.join(BUILD, f"fasta-{n}.txt")
    if not os.path.exists(path):
        cmd, _ = build("fasta", "c")
        with open(path + ".tmp", "wb") as f:
            subprocess.run(cmd + [str(n)], stdout=f, check=True)
        os.replace(path + ".tmp", path)
    with open(path, encoding="latin-1") as f:
        return f.read()


def cmd_run(args):
    if not os.path.exists(FUNK):
        sys.exit("bench: ./funk not found; run make first")
    names = args.only.split(",") if args.only else list(SIZES)
    backends = (args.backends or ",".join(DEFAULT_BACKENDS)).split(",")
    for name in names + backends:
        if name not in SIZES and name not in BACKENDS:
            sys.exit(f"bench: unknown benchmark or backend `{name}`")
    meta = run_meta(args.note)
    rows = []
    for bench in names:
        n = SIZES[bench]
        stdin = stdin_for(bench, n)
        outputs = {}
        for backend in backends:
            try:
                cmd, build_s = build(bench, backend)
                if backend == "c":
                    cmd = cmd + [str(n)]  # the Game's C reads n from argv; the core suite's ignores it
                out = measure(cmd, stdin)[0]  # warm-up, also the output check
                outputs[backend] = hashlib.sha256(out.encode("latin-1")).hexdigest()  # fasta prints 100 MB
                samples = [measure(cmd, stdin) for _ in range(args.repeat)]
            except RuntimeError as e:
                print(f"{bench:13} {backend:9} FAILED: {e}", file=sys.stderr)
                continue
            row = dict(meta, bench=bench, backend=backend, n=n, build_s=build_s,
                       wall=[round(s[1], 4) for s in samples], cpu=[round(s[2], 4) for s in samples],
                       maxrss_kb=max(s[3] for s in samples))
            rows.append(row)
            print(f"{bench:13} {backend:9} {min(row['wall']):8.3f}s  {row['maxrss_kb'] // 1024:6d} MiB", flush=True)
        if len(set(outputs.values())) > 1:
            print(f"{bench:13} output differs between backends; not recorded", file=sys.stderr)
            rows = [r for r in rows if r["bench"] != bench]
    with open(RESULTS, "a") as f:
        for r in rows:
            f.write(json.dumps(r) + "\n")
    print(f"appended {len(rows)} rows to {os.path.relpath(RESULTS, ROOT)} (run {meta['run']})")


def load(host):
    rows = []
    try:
        with open(RESULTS) as f:
            rows = [json.loads(l) for l in f if l.strip()]
    except FileNotFoundError:
        pass
    return [r for r in rows if r["host"] == host]


def by_run(rows):
    """{run id: {(bench, backend): row}}, ordered by run id (an ISO timestamp)."""
    runs = {}
    for r in sorted(rows, key=lambda r: r["run"]):
        runs.setdefault(r["run"], {})[(r["bench"], r["backend"])] = r
    return runs


def label(row):
    return row["commit"][:8] + ("+" if row["dirty"] else "")


def cmd_report(args):
    runs = by_run(load(args.host))
    if not runs:
        sys.exit(f"bench: no results for host {args.host}")
    ids = list(runs)
    cur = runs[ids[-1]]
    if args.baseline:
        base_ids = [i for i in ids[:-1] if next(iter(runs[i].values()))["commit"].startswith(args.baseline)]
        if not base_ids:
            sys.exit(f"bench: no run of commit {args.baseline} on {args.host}")
        base = runs[base_ids[-1]]
    else:
        base = runs[ids[-2]] if len(ids) > 1 else {}
    first = next(iter(cur.values()))
    print(f"run {ids[-1]} {label(first)} on {first['machine']}")
    if base:
        b0 = next(iter(base.values()))
        print(f"baseline {b0['run']} {label(b0)}; metric: min wall time; threshold {args.threshold:.0%}")
    print(f"\n{'bench':13} {'backend':9} {'time':>8} {'x C':>7} {'base':>8} {'change':>8}")
    regressed = 0
    for (bench, backend), r in sorted(cur.items(), key=lambda kv: (list(SIZES).index(kv[0][0]) if kv[0][0] in SIZES else 99, BACKENDS.index(kv[0][1]))):
        t = min(r["wall"])
        c = cur.get((bench, "c"))
        ratio = f"{t / min(c['wall']):6.1f}x" if c and backend != "c" else ""
        line = f"{bench:13} {backend:9} {t:7.3f}s {ratio:>7}"
        b = base.get((bench, backend))
        if b and b["n"] == r["n"]:
            change = t / min(b["wall"]) - 1
            flag = "  REGRESSION" if change > args.threshold else "  improved" if change < -args.threshold else ""
            regressed += change > args.threshold
            line += f" {min(b['wall']):7.3f}s {change:+7.1%}{flag}"
        print(line)
    for title, run in (("run", cur), ("baseline", base)):
        g = geomean(run)
        if g:
            target = "; target 2x" if SUITE == "core" else ""
            print(f"\ngeomean compiled/C ({title}): {g:.2f}x over {g_count(run)} benchmarks{target}", end="")
    print()
    sys.exit(1 if regressed else 0)


def g_count(run):
    return sum(1 for (b, k) in run if k == "compiled" and (b, "c") in run)


def geomean(run):
    """Geometric mean of compiled over C min wall times: the performance goal."""
    rs = [min(r["wall"]) / min(run[(b, "c")]["wall"]) for (b, k), r in run.items() if k == "compiled" and (b, "c") in run]
    return math.exp(sum(map(math.log, rs)) / len(rs)) if rs else None


def cmd_history(args):
    runs = by_run(load(args.host))
    print(f"{'run':25} {'commit':9} {'n':>9} {'time':>8} {'x C':>7} {'MiB':>6}")
    for rid, rs in runs.items():
        r = rs.get((args.name, args.backend))
        if not r:
            continue
        c = rs.get((args.name, "c"))
        ratio = f"{min(r['wall']) / min(c['wall']):6.1f}x" if c and args.backend != "c" else ""
        print(f"{rid:25} {label(r):9} {r['n']:9} {min(r['wall']):7.3f}s {ratio:>7} {r['maxrss_kb'] // 1024:6}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--suite", default="core", choices=list(SUITES))
    sub = ap.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("run")
    r.add_argument("-r", "--repeat", type=int, default=5, help="timed runs per benchmark and backend")
    r.add_argument("--only", help="comma-separated benchmark names")
    r.add_argument("--backends", help="comma-separated; default c,compiled,interp (game: c,compiled)")
    r.add_argument("--note", default="", help="free text stored with the run")
    for name in ("report", "history"):
        p = sub.add_parser(name)
        p.add_argument("--host", default=platform.node())
        if name == "report":
            p.add_argument("--baseline", help="commit prefix to compare against")
            p.add_argument("--threshold", type=float, default=0.10)
        else:
            p.add_argument("name")
            p.add_argument("--backend", default="compiled", choices=BACKENDS)
    args = ap.parse_args()
    global SUITE, PROGS, SIZES, RESULTS, DEFAULT_BACKENDS
    SUITE = args.suite
    PROGS, SIZES, RESULTS, DEFAULT_BACKENDS = SUITES[SUITE]
    if args.cmd == "history" and args.name not in SIZES:
        sys.exit(f"bench: unknown benchmark `{args.name}` in the {SUITE} suite")
    {"run": cmd_run, "report": cmd_report, "history": cmd_history}[args.cmd](args)


if __name__ == "__main__":
    main()
