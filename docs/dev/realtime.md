# Real time: design note

Status: R0 and R1 implemented, 2026-10-26 (see "Implementation"); R2-R4 are proposals. It bounds the collector's pauses for any real-time use: audio, MIDI, games and control loops, not MIDI only. It replaces "Pauses" in [ffi.md](ffi.md#pauses).

## Requirements

| Use | Deadline | Thread | What a pause breaks |
|-|-|-|-|
| Audio | one buffer: 1.3 ms at 64 frames and 48 kHz, 10.7 ms at 512 | the audio API's, high priority | a dropout, audible |
| MIDI | about 1 ms of timing jitter | in: the backend's; out: funk's | late notes |
| Games, UI | a frame: 16.7 ms at 60 Hz, 8.3 ms at 120 Hz | funk's | a dropped frame |
| Control loops | the period: 1 ms at 1 kHz | funk's, or a timer's | a missed sample |

Two classes follow:

- **Soft real time, on funk's thread:** MIDI out, games, UI, most control loops. They need pauses well under their deadline, about 1 ms or less, at any heap size.
- **Hard real time, on a foreign thread:** audio callbacks, and any callback a driver calls with a deadline. The thread must not block or take a lock that funk holds, and it must not wait for a collection at all. No pause is short enough.

## Today, measured

An instrumented copy of the runtime timed each collection of two synthetic programs (`-O2`, M1, 2026-10-26). `churn` keeps a list of n cells live while a loop allocates garbage; `deep` allocates at the bottom of a recursion n frames deep. Each row is the longest collection of the run.

| Program | n | Roots (stack and tables) | Mark | Sweep | Pause |
|-|-|-|-|-|-|
| `churn` | 100,000 cells | 0.0 ms | 1.5 ms | 16.4 ms | 17.9 ms |
| `churn` | 1,000,000 cells | 0.0 ms | 10.1 ms | 24.8 ms | 35.0 ms |
| `churn` | 4,000,000 cells | 0.0 ms | 68.6 ms | 9.6 ms | 78.2 ms |
| `deep` | 1,000 frames | 0.0 ms | 0.0 ms | 14.8 ms | 14.8 ms |
| `deep` | 100,000 frames | 1.6 ms | 0.8 ms | 17.3 ms | 19.7 ms |
| `deep` | 1,000,000 frames | 16.4 ms | 6.8 ms | 11.9 ms | 35.0 ms |

- **Sweep:** 10-25 ms, about 0.1 ms per MiB of heap. The heap is at least 128 MiB, since a collection waits for that much allocation ([memory.md](memory.md#after-workers-and-fusion-2026-10-25)). It is the whole pause for small live data.
- **Mark:** about 10-17 ms per million live cells, so it grows with the program's state.
- **Stack:** about 16 us per 1,000 frames, scanned conservatively word by word.

Every pause is over every deadline in the table above; the shortest, 14.8 ms, is over all but the 60 Hz frame.

## What funk's semantics give

Values are immutable after construction. Only four code paths write into an object that already exists:

| Write | Where | Old value |
|-|-|-|
| a thunk's update with its value | `rt.c`, `fk_force` | the thunk's code and environment |
| TRMC's `*dst = v`, the tail of the last cell | `cgen.c` (`cg_tail`), `rt.c` (`split`, for `words` and `lines`) | a placeholder, or the old tail when the cell is reused |
| cell reuse, `FK_FIELD(cell, 0) = x` | `cgen.c`, from [reuse.md](reuse.md) | the dead cell's head |
| a recursive closure's free variables, filled after allocation | `cgen.c` (`closure_fill`), `interp.c` | nothing: the closure is new |

All other stores initialize a new object. The globals (`G`, `gvals`) and the `StablePtr` table are roots, written outside the heap.

The collector does not move objects, and C may hold pointers into the heap (`String` arguments, `StablePtr` contents), so a moving or copying design is out.

## Options

| | Design | Pause | Cost | Work |
|-|-|-|-|-|
| A | Lazy sweeping: allocation sweeps a chunk when its size class runs out | removes sweep: roots + mark | none measurable expected; sweep is spread, not removed | small |
| B | A, plus incremental marking: snapshot at the beginning, with a deletion barrier, in steps paced by allocation | roots + one step | a barrier at the four writes; a branch in allocation while marking | medium |
| C | B, plus incremental stack scanning (stack barriers) | one step | return-address patching; hard with conservative frames | large |
| D | Concurrent marking on a second thread | roots | B's barrier, plus a thread-safe mark state | large |
| E | Generational, non-moving (sticky mark bits) | frequent short young collections; full ones still pause | a barrier on old-to-young stores | medium |
| F | Reference counting (Perceus) | none, except cascading frees | measured slower: `lists` 136 ms against 70 ms ([memory.md](memory.md#perceus-prototyped-by-hand)) | large |
| G | Allocation-free real-time functions, exported to C | no interaction with the collector | a checked subset of funk | medium |

Notes:

- **B's barrier is cheap because funk mutates little.** Snapshot at the beginning keeps every object reachable when marking starts. Only overwrites can hide one, so only the four writes above log their old value, and initializing stores, nearly all of them, need nothing. Objects allocated while marking are allocated marked. An insertion barrier (Dijkstra's) would instead have to cover every initializing store into a new marked object, so it suits funk less.
- **The stack stays a pause in A and B.** Snapshot at the beginning scans the stack when marking starts: 16 us per 1,000 frames. Real-time code typically runs from a shallow event loop; a loop 1,000 frames deep pauses for under 0.1 ms for its stack. C removes this term but patches return addresses in frames the conservative scan cannot parse; it is not proposed.
- **D needs B's barrier anyway, and a second core.** It does not shorten the pause below B's roots term. Not proposed.
- **E lowers the collector's total cost, not its worst pause.** A full collection still marks the whole heap. It is a later complement to B, for throughput.
- **F is ruled out by measurement:** freeing scatters later allocations and costs locality ([memory.md](memory.md#perceus-prototyped-by-hand)).
- **No collector serves hard real time on a foreign thread.** A collection on funk's thread can free an object that only the audio thread's stack holds, since the collector scans one stack; and the audio thread cannot wait for funk's. G serves that class by never touching the heap.

## Recommendation

B for soft real time, G for hard real time, and two small supporting changes. In order:

1. **R0: pause statistics.** `FUNK_GCSTATS=1` prints each collection's roots, mark, sweep and pause times, and a summary at exit. Every later step is measured with it. Small.
2. **R1: lazy sweeping (A).** After marking, every chunk is unswept. Allocation that finds its free list empty sweeps the next unswept chunk of that size class, and a collection first finishes any sweep still pending. Expected: pauses of roots + mark, 1.5 ms and 10.1 ms for the first two `churn` rows. A hypothesis: total time unchanged.
3. **R2: incremental marking (B).** A collection becomes a cycle: scan the roots and push them (the pause), then mark in steps of a fixed budget, one step per k bytes allocated, then rescan the globals and the `StablePtr` table and finish (a second, short pause). The four writes call a barrier while a cycle is marking: `if (fk_marking) fk_log(old)`. Allocation while marking sets the new object's mark. Pacing starts a cycle early enough that marking ends before the heap must grow; k follows from the live size. Target: pause at most 0.5 ms plus 16 us per 1,000 frames of stack, at any heap size.
4. **R3: a lock-free event ring.** Queued trampolines write into a fixed-size ring of fixed-size events, one producer slot claimed with an atomic increment, instead of `malloc` and a mutex. A full ring drops the event and counts it. Byte buffers longer than an event's inline space (64 bytes, a parameter) fall back to `malloc`, or are dropped in a strict mode. This bounds the cost a callback adds to a foreign real-time thread.
5. **R4: allocation-free exports (G).** `foreign export ccall "realtime" process :: Ptr () -> Ptr CFloat -> CInt ->> ()` exports a top-level function as a C function with its typed worker's parameters, callable from any thread. The checker proves it allocation-free: its body uses scalar arithmetic, calls to other such functions, `peek` and `poke` at scalar types, and C imports; no constructors, closures, boxing or strings. State lives in C memory, reached through `Ptr`, and is exchanged with funk's thread through R3's ring. It needs unboxed `peek` and `poke` in the typed backend; today `peek` on a `Ptr CFloat` boxes its result (`fk_p_peekf` calls `fk_float`). The interpreter cannot run such an export, since it boxes every value; a program that uses one must be compiled.

R1 and R2 change only the runtime and four code-generation sites. R4 adds a checker pass and a typed-backend form, and is the only step visible in the language.

## Implementation: R0 and R1

- **R0.** `FUNK_GCSTATS=1` prints a line per collection and a summary at exit, in both backends and for `funk` itself:

  ```text
  gc 2: pause 8.68 ms (roots 0.01, mark 8.67, finish 0.00), live 9.6 MiB, heap 141 MiB
  gc: 2 collections, pause max 8.68 ms, mean 7.46 ms, total 14.93 ms; lazy sweep 248 chunks, max 0.087 ms, total 8.69 ms
  ```

  Times come from `clock_gettime` around each phase, read only when the variable is set. "Finish" is the large-object sweep and the rebuilding of the unswept lists.
- **R1.** As proposed in A, with three details:
  - A chunk left unswept when the next collection starts would keep its old marks, which read as already marked and would hide live objects. Its mark bytes are cleared instead, one `memset` of a byte per object; the new collection then frees what died since. Sweeping it would put its cost back in the pause.
  - The live size, which sets the next threshold, is counted while marking.
  - A chunk whose objects are all marked has nothing to free. Marking counts marked objects per chunk, and such a chunk only has its marks cleared. Without this, one allocation swept every full chunk of a long-lived list in a row: 3.1 ms at 4,000,000 live cells.

Pauses, the longest collection of each run, before and after (the programs of "Today, measured"):

| Program | n | Before | After | Longest lazy sweep step |
|-|-|-|-|-|
| `churn` | 100,000 cells | 17.9 ms | 0.98 ms | 0.14 ms |
| `churn` | 1,000,000 cells | 35.0 ms | 9.8 ms | 0.15 ms |
| `churn` | 4,000,000 cells | 78.2 ms | 37.5 ms | 0.15 ms |
| `deep` | 1,000 frames | 14.8 ms | 0.09 ms | 0.12 ms |
| `deep` | 1,000,000 frames | 35.0 ms | 23.0 ms | 0.10 ms |

What remains is marking and the stack, which R2 addresses. A sweep step costs at most one chunk, about 0.15 ms.

Throughput: the same emitted C, with lazy sweeping and with every chunk swept eagerly at the collection, 15 interleaved runs each, minimum wall time: `bst` 293.0 against 298.1 ms, `trees` 100.1 against 103.6, `sort` 63.6 against 69.1, `lists` 53.3 against 55.0. Lazy sweeping is 2-8% faster; the hypothesis was no change. A likely reason, not measured: an object is swept just before its reuse, so it is still in cache. The eager variant keeps the per-chunk mark count, so it is close to the old collector, not identical to it. The benchmark suite (`bench/results.jsonl`, runs of 2026-10-26 13:29 and 13:43) shows no change beyond noise.

Tests: `tests/run/gc.fk` collects several times while a 200,000-cell list stays live, in both backends, and `tests/run.sh` checks the format of `FUNK_GCSTATS`'s output.

## Questions

1. **Is R4's restriction acceptable?** Real-time audio code in funk would be a first-order, allocation-free subset: loops over `Ptr CFloat` buffers, with state in C memory. Real-time audio code in C avoids allocation, locks and I/O in the callback for the same reason ([Bencina, "Real-time audio programming 101"](http://www.rossbencina.com/code/real-time-audio-programming-101-time-waits-for-nothing)), but it is not idiomatic funk. The alternative, a region per callback, freed when the callback returns, would allow allocation inside a callback, but needs a check that nothing allocated there escapes.
2. **What pause target?** 0.5 ms plus the stack term covers MIDI, games and 1 kHz control loops. Audio relies on R4 either way.
3. **Order:** R0 and R1 are independent and small; R2 builds on R1. R3 and R4 are independent of the collector. Proposed: R0, R1, R2, then R3 and R4.

Generational collection (E) is left for throughput after R2.
