# Memory: design note

Status: TRMC implemented, released in 0.2.0. After it, a profile rules out B; C is deferred (see "After TRMC"). It covers step 6 of [performance-first.md](performance-first.md): the cost of allocation and collection. The geometric mean is 2.4x against the 2x target. The largest ratios left are `lists` 8.0x, `queens` 5.4x, `sort` 4.7x and `strings` 4.0x, all allocation-heavy.

## Where the time goes

Samples of compiled `lists`, on an M1:

| Function | Share |
|-|-|
| funk code, allocation inlined | about 55% |
| `scan_range` (marking) | about 25% |
| `fk_gc` (mostly sweeping) | about 15% |

Most of the marking is the stack. `map`, `filter` and `enumFromTo` recurse once per element, so a list of 300,000 elements leaves 300,000 frames on the stack while it is built. Each collection scans all of them conservatively, word by word. The C program builds the same lists in loops, with a pointer to the last cell's tail.

## Experiment: building lists in loops

In the generated C for `lists`, `enumFromTo@Int`, `filter@{even}` and `map@{...}` were rewritten by hand as loops. Each allocates a cell with an empty tail, stores it through a destination pointer, and moves the pointer to the new cell's tail:

```c
V head = fk_nil, *dst = &head;
for (; FK_CONID(l) == FK_C_CONS; l = FK_FIELD(l, 1)) {
  V cell = fk_mkcon(FK_C_CONS, 2, (V[]){f(FK_FIELD(l, 0)), fk_nil});
  *dst = cell;
  dst = &FK_FIELD(cell, 1);
}
return head;
```

| | Time | Ratio to C |
|-|-|-|
| as generated | 176 ms | 8.0x |
| three functions as loops | 70 ms | about 3.1x |

The stack stays shallow, so marking and the per-element call overhead mostly disappear. This is tail recursion modulo cons (TRMC). OCaml has it as `[@tail_mod_cons]` ([Bour, Clément and Scherer, 2021](https://arxiv.org/abs/2102.09823)), and Koka applies it automatically.

## Options

| Option | Measured or expected | Cost | Note |
|-|-|-|-|
| A. TRMC in `cgen.c` (chosen first) | `lists` 2.5x faster, measured by hand | about 150 lines; code generation only | also removes the stack limit for `map`-like functions |
| B. Bump-allocated nursery, mostly-copying minor collections | allocation becomes a pointer bump; short-lived cells are never swept | large: copying with pinned pages for conservative roots; a write barrier where young values are written into old objects (thunk updates, TRMC destinations) | the stack is still scanned at each minor collection, so it needs A to pay off |
| C. Perceus reference counting with in-place reuse | `map` over a list used once allocates nothing | very large: reference-count operations throughout generated code, the interpreter and the runtime; cycles from recursive local closures must be removed by lambda lifting | the largest gain, the largest change |
| D. Fusion (`foldr`/`build`) | `sum (map f (filter p xs))` becomes one loop with no list | large: rewrite rules and inlining | removes the lists that the C benchmarks keep; helps only pipelines |

A comes first. It is measured, small and independent, and it makes B cheaper, since B's minor collections would otherwise scan the same deep stacks. B or C is decided after A, from a new profile.

## TRMC: design

**Candidates.** A function is a candidate if, in some tail position, its result is a constructor application with a saturated self-call as one argument: `x : map f xs`, `merge a b : mergePairs rest`. In the prelude this covers `map`, `filter`, `++`, `takeWhile`, `enumFromTo`, `enumFromThenTo`, `zip`, `zipWith`, `replicate`, `take`, `init`, `mergePairs` and `merge`, plus their specialization copies.

**Evaluation order.** Evaluation is strict, left to right. In `C a (f xs) b`, `b` is evaluated after the recursive call. A loop would evaluate it before. The transformation therefore applies only if every argument after the self-call is safe to evaluate early, by the same rule as eager `let`. For `x : f xs`, the self-call is the last argument, so this never limits it.

**Code.** The function keeps its signature. In its body:

- Two locals: `head`, which receives the first result, and `dst`, which points to where the next result goes. `dst` starts as `&head`.

- A tail-position constructor application with the self-call allocates the constructor, with a placeholder in the self-call's field, and stores it through `dst`. It then points `dst` at that field and jumps to the top with the self-call's arguments, as self tail calls already do.

- Every other tail-position result is stored through `dst`, and the function returns `head`.

Self tail calls in other branches, such as `filter` skipping an element, keep jumping to the top with `dst` unchanged.

**Collector.** The non-moving collector sees `head` on the stack, and every cell is reachable from it through tails that have been written. `dst` is an interior pointer, which the collector already accepts. A collection between allocating a cell and the loop's next store finds the placeholder, which is an ordinary value. Option B would need a write barrier on the store through `dst`, because the cell may have been promoted in between.

**Semantics.** The results are unchanged. Two behaviours change for the better: a long `map` no longer uses stack proportional to its length, so it can no longer overflow; and an error raised partway through leaves only an unreachable partial list.

**Interpreter.** Not changed. It still recurses, which is correct and only slower; interpreter speed is not part of the goal.

## Result

The implementation requires the self-call to be the constructor's last argument, so no evaluation-order check is needed. All prelude candidates have that shape. The geometric mean went from 2.40x to **2.06x**, against an estimate of about 2.1x:

| Benchmark | Before | After |
|-|-|-|
| `lists` | 8.0x | 3.2x |
| `sort` | 4.7x | 3.1x |
| `strings` | 4.0x | 3.8x |
| `queens` | 5.4x | 5.8x |

`queens` is about 3.5% slower in an interleaved A/B run. Its lists are 11 elements long, so the loop's setup costs more than the stack it saves. `trees` also becomes a loop on its right spine (`Node (make d) (make d)`), with no measurable change. Profiles after TRMC decide between B and C.

## After TRMC

Each collection's phases, timed in an instrumented build at the benchmark sizes:

| Benchmark | Total | Collections | Marking | Sweep | Stack scan |
|-|-|-|-|-|-|
| `lists` | 69 ms | 2 | 11.5 ms | 13.6 ms | 0 ms |
| `strings` | 126 ms | 1 | 41.3 ms | 4.2 ms | 0 ms |
| `sort` | 107 ms | 2 | 23.3 ms | 12.8 ms | 0 ms |

Collection is about 35% of these programs, in one or two collections. Stack scanning is gone. What remains is marking live data and sweeping.

- **B is ruled out.** At each collection, the live data is a list or string built just before: young, and still live. A minor collection would mark it all the same.

- **The collector is near its limit.** A reciprocal in place of the division per marked word saved 3-8% of marking, and was kept. Marking follows millions of cells and pays a cache miss for each; a tracing collector pays this in proportion to live data.

- **C, Perceus, is the option that removes marking and sweeping.** Its cost is unchanged: reference-count operations in generated code, the interpreter and the runtime, and lambda lifting of the remaining recursive closures to avoid cycles. It is deferred: the geometric mean is already 1.93x, under the target, and C is the largest change on the list. It needs its own design pass before any code.

## Perceus, prototyped by hand

Before implementing C, its effect was measured on `lists`. The generated `filter`, `map` and `foldl` copies were rewritten by hand as Perceus would compile them. The reference count is the header's spare byte, and a cell is freed or reused only when the count shows it unique. All cells in this program are unique, so the prototype needs none of the increments a real implementation would add; it is an upper bound.

| Variant | `lists` (C: 22 ms) |
|-|-|
| tracing collector, today | 70 ms |
| reuse in place only (`filter` and `map` write their output into their input's cells) | **54 ms** |
| freeing only (cells freed as consumed, onto the free lists) | 152 ms |
| both, as Perceus does | 136 ms |
| both, freed cells kept in an array stack instead of linked free lists | 109 ms |

Freeing makes the program slower, with either free-list layout. With the tracing collector, consecutive allocations come from swept or fresh memory in address order, so a list's cells are contiguous and every traversal reads memory sequentially. Cells freed as they die are reused in the order they died, so the next list is scattered, and every traversal of it misses the cache. Reuse in place has no such cost: the output occupies exactly the input's cells, in order.

Consequences:

- **Perceus as designed is not implemented.** It would need an allocator that keeps locality under explicit freeing (mimalloc's per-page free lists are one approach) before it could pay off. That is a second large project on top of the first.

- **The gain is in reuse, about 20% on `lists`** (18-23%, with only `filter` and `map` changed). Reuse needs to know that a cell is unique. Full reference counting gives that, at the cost of increments and decrements throughout generated code, which the prototype did not include. A cheaper route: a static check that an argument is a fresh list, such as the direct result of a TRMC-built call like `filter p (enumFromTo 1 n)`, passed to a parameter the callee uses only once. The static route is implemented: [reuse.md](reuse.md).

## After workers and fusion (2026-10-25)

An instrumented copy of the runtime timed each collection's phases at the benchmark sizes:

| Benchmark | Collections | Marking | Sweep | Share of run |
|-|-|-|-|-|
| `lists` | 1 | 1.3 ms | 10.0 ms | 12% |
| `sort` | 1 | 2.8 ms | 6.4 ms | 9% |
| `trees` | 2 | 2.8 ms | 15.4 ms | 13% |
| `strings`, `queens`, `closures` | 0 | | | 0% |

The earlier figure of 65% for `lists` predates fusion, reuse, typed workers and the rewritten `lists` program. Sweep is now most of the collector's time. The share grows with input size, since live data does: at 4x the sizes, `sample` puts the collector at about 50% of `lists`, 35% of `sort` and 29% of `trees`.

Two hypotheses were tested:

- **Page faults from the 128 MiB threshold.** `lists` touches 140 MiB, where its C version touches 12 MiB. Lowering the threshold made every allocating benchmark slower: geometric mean +2.5% at 32 MiB, +15% at 4 MiB (`trees` +58%). More collections cost more than the faults they save. 64 MiB ties with 128 MiB. Rejected.

- **Free lists built in advance.** A new chunk pushed every object onto its free list, writing each one before its first use. Bump allocation through fresh chunks, with sweep ending the bump region, measured -3.0% on the geometric mean (`lists` -9%, `trees` -4%). Kept.

What remains in the collector is sweep and marking. They are proportional to heap size and to live data, so they matter most for programs larger than these benchmarks. Lazy sweeping, which sweeps a chunk when allocation reaches it, would spread the cost but not reduce it. It is not started.

The OCaml citation is from memory and was not checked in this session.
