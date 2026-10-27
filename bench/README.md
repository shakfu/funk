# Benchmarks

Each benchmark exists as `progs/NAME.fk` and `progs/NAME.c`. They are timed on three backends:

| Backend | Command |
|-|-|
| `c` | `$CC -O2 NAME.c` |
| `compiled` | `funk build NAME.fk` |
| `interp` | `funk NAME.fk` |

```sh
make bench                                  # all benchmarks, 5 timed runs each
python3 bench/bench.py run --only fib,trees --backends compiled -r 10
python3 bench/bench.py report               # latest run vs the previous one
python3 bench/bench.py report --baseline 690ac24 --threshold 0.05
python3 bench/bench.py history trees --backend interp
```

`report` exits 1 if any benchmark is slower than its baseline by more than the threshold (default 10%). It needs Python 3; nothing outside the standard library.

## What the C programs are

The C programs use the same algorithm and data structures as the funk programs: linked lists, recursion, closures as a code pointer plus an environment, packed strings. Where the compiler removes a data structure, as fusion does in `pipeline`, the C version does without it too. Rows named `lists` before fusion measured what `pipeline` measures now; the new `lists` has a different size, so `report` does not compare them. `strings` used `[Char]` lists in both versions until `String` became packed; rows before that change are not comparable. Numbers are formatted with a digit loop (`put_decimal` in `progs/arena.h`), as funk's `show` at `Int` is; with `snprintf`, the C versions of `printing`, `strings` and `wordfreq` measured libc's formatting, and their C rows before 2026-10-27 are not comparable. They allocate from a bump arena (`progs/arena.h`) that is reset between phases, in place of a GC. Idiomatic C with arrays would mostly measure lists against arrays. That is a language design choice, which optimizations of funk cannot change.

All backends must print the same output. A benchmark whose outputs differ is not recorded.

## Benchmarks

| Name | Measures | n |
|-|-|-|
| `fib` | non-tail calls, Int arithmetic | argument |
| `loop` | self tail call over Int arithmetic | iterations |
| `lazywhere` | one thunk per iteration for a `where` binding | iterations |
| `floats` | boxed Float arithmetic (midpoint integration) | steps |
| `lists` | `map`, `filter`, `foldl` over fresh lists, each named so fusion cannot apply, 20 rounds | list length |
| `pipeline` | `sum (map f (filter p (enumFromTo 1 n)))`, 20 rounds; fused into a loop, and the C version is that loop | list length |
| `sort` | prelude merge sort of pseudo-random Ints | list length |
| `queens` | all n-queens solutions as lists of partial boards | board size |
| `trees` | binary trees from the Benchmarks Game: allocation and GC with a live tree | max depth |
| `closures` | unknown calls through closures, a partial application and a composition | calls per function |
| `strings` | `show`, `unwords` and `words` over packed `String`; the C version uses `char` buffers | count of numbers |
| `records` | particles as records with `Float` fields, advanced by record update, 100 steps | particles |
| `bst` | a persistent binary search tree as a map: path-copying inserts, then lookups | keys |
| `wordfreq` | build text, split it into words, merge sort, count runs: `String` comparison and `++` | words |
| `printing` | one `putStrLn` per line, with `show` and `++`: an effect-heavy loop | lines |

Sizes are in `SIZES` in `bench.py`. Each size keeps the interpreter under about 3 s. On an M1, C times are therefore 5-75 ms, so `x C` ratios carry several percent of noise. Changing a size makes older rows incomparable, and `report` skips rows whose `n` differs.

## Benchmarks Game suite

`game/` holds programs from [The Computer Language Benchmarks Game](https://benchmarksgame-team.pages.debian.net/benchmarksgame/): each task's funk version, `NAME.fk`, and one of the Game's own C programs, `NAME.c`, under its license (`game/LICENSE`). The core suite pairs funk with C written to match it; this suite measures funk against C written for the task, with the Game's rules: the same algorithm, any data structure.

```sh
python3 bench/bench.py --suite game run     # c and compiled; results in game-results.jsonl
python3 bench/bench.py --suite game report
```

- **Which C.** The plainest single-threaded C program of each task without the Game's asterisk: `gcc-1`, and `gcc-8` for spectral-norm and mandelbrot, their only ones. The fastest C entries use threads and SIMD, which funk has not.
- **Sizes.** Below the Game's, so that C takes 0.4-2.8 s on an M1 (`GAME_SIZES` in `bench.py`). The C program reads n from `argv[1]`, funk from stdin.
- **Checks.** `NAME.expected` is the Game's output file for the small n in `NAME.stdin`; `make test` runs each program on it, interpreted and compiled (label `game`). `run` also compares the two backends' output at the full size.
- **Flags.** `-O2` for both, as in the core suite; the Game builds C with `-O3 -march`.

| Task | Size | Notes |
|-|-|-|
| `nbody` | 10,000,000 steps | bodies as a list of records with unboxed `Float` fields, updated pairwise in C's order, so the floating-point results are C's |
| `spectralnorm` | 3,000 | `Array Float` vectors, the four functions of the Java program |
| `fannkuchredux` | 11 | `Array Int` permutations, C's generation order |
| `binarytrees` | depth 18 | the collector against C's `malloc` and `free` per node |
| `fasta` | 10,000,000 | the same generator and binary search; one `putStrLn` per 60-character line, as C's `puts` |
| `mandelbrot` | 4,000 | the same iteration, in C's order of operations; a binary PBM, one `putStr` per row of bytes |

## Data

`results.jsonl` is append-only, one JSON object per benchmark, backend and run. Commit it to keep the history.

| Field | Meaning |
|-|-|
| `run` | UTC start time of the run; groups its rows |
| `commit`, `dirty` | `HEAD`, and whether the tree had changes other than `results.jsonl` |
| `host`, `machine`, `cc` | where it ran; `report` and `history` compare one host only |
| `note` | free text from `--note` |
| `bench`, `backend`, `n` | what ran |
| `wall`, `cpu` | per-run wall and user+system seconds, after one untimed warm-up run |
| `maxrss_kb` | largest resident set over the timed runs |
| `build_s` | `funk build` or `cc` time; null for `interp` |

Comparisons use the minimum wall time, the sample least affected by other load. Every sample is stored, so other statistics can be computed later.

Timings include process start-up. For `interp` that includes compiling the prelude, about 3 ms on an M1.
