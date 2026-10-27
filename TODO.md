# TODO

Always organize according to sections descending priority (Critical, High, Medium, Low). The can include which can include subsections as required for organization.

The primary goal is generated C speed: a geometric mean of 2x over C on the benchmark suite. It is 1.19x now (2026-10-27); the remaining items lower it further. See [docs/dev/performance-first.md](docs/dev/performance-first.md).

## Critical

- [ ] **Typed backend.** C scalars (`int64_t`, `double`, `bool`) in monomorphic code, `V` at polymorphic boundaries, workers with typed parameters, unboxed scalar fields. Design: [docs/dev/typed-backend.md](docs/dev/typed-backend.md). Steps 1-6 are done.
- [ ] 64-bit `Int` at `V` boundaries now costs at most 0.7% (geometric mean, 6 benchmarks; `queens` 3%): removing the range check and the boxing altogether gains 2.7%, and `__builtin_expect` on both tests recovered 2.0. Typed list heads would remove the rest; [docs/dev/typed-backend.md](docs/dev/typed-backend.md#integers).
- [x] Copies made by `ho_specialize` get the call site's ground types: `flip@{0=primMulInt}` takes `int64_t`. Geometric mean -5.7% (`pipeline` -47%, `closures` -4%, `sort` +2.5%).
- [x] Nullary constructors as constants in generated C. Interleaved, 21 runs: `lists` -9.6%, `sort` -5.4%, `wordfreq` -4.9%, `trees` -1.9%, others within 0.5%.

## High

- [x] **In-place reuse of unique list cells.** Done: 1.79x to 1.68x ([docs/dev/reuse.md](docs/dev/reuse.md#result)).

- [x] **List fusion.** Done: 1.68x to 1.52x; `queens` 2.9x to 1.4x ([docs/dev/fusion.md](docs/dev/fusion.md#result)).
- [ ] Fusion: `enumFromThenTo` as a producer, and nested `concatMap`.

- [x] Generated C compiles without `-Wall -Wextra` warnings; the `wall` tests check every run test and example.

## Medium

- [ ] **Real time, what remains** ([docs/dev/realtime.md](docs/dev/realtime.md)): no test proves the stack snapshot's page copy necessary since protection stops below the thread's first frame (`tests/run/gcdeep.fk` passes without it); the Linux limit is from glibc's layout, not run on Linux. An export's stack bound is not computed; for targets with small stacks, sum `-fstack-usage` frame sizes along the call graph. `foreign struct` has no C arrays inside structs (Core Audio's `AudioBufferList`).

- [ ] `closures` is 1.7x C. Calling an exact-arity closure inline instead of through `fk_apply`, and a partial application's call without its variable-length array, moved samples out of `fk_apply` but left the time unchanged (-0.6%, reverted). The loop is a chain of dependent `mod`s; on it, funk adds a box, an unbox and a store and reload through the `V *` argument array, which C passes in a register (inferred, not measured). A typed entry point in closures of common shapes (`Int -> Int`), called when the call site's types match, would take the value off memory.

- [ ] **Benchmarks Game, phases 2-4** (`bench/game/`, [bench/README.md](bench/README.md#benchmarks-game-suite)). Phase 1 has 5 tasks, geometric mean 1.75x C (2026-10-27). Next:
  - mandelbrot: needs bitwise primitives (`shiftL`, `.|.`) and binary output.
  - reverse-complement and k-nucleotide: need reading stdin to its end, and a hash table.
  - regex-redux and pidigits: POSIX `regex.h` and GMP through the FFI.
- [ ] Benchmarks Game, phase 1 gaps:
  - `nbody` is 4.0x C: each pair update builds two `Body`s and a tuple, and each step a list, where C updates five structs in place.
  - `fannkuchredux` is 3.0x C. It no longer allocates (a thunk for `let m = max maxFlips f` was 1.4 GB over 40M permutations; 135 to 2 MB resident); each `Array` access is still a call with a bounds check.
  - `fasta` is 2.4x C: each 60-character line is a `[Char]` packed into a `String`.
- [ ] The effect checker's "declared with `->` but calls an effectful function" names the function, not the arrow. A function that acts and calls a parameter needs `->>` on each of that parameter's arrows (`(Int ->> Int ->> Float)`); a definition with fewer parameters than its signature's arrows reaches a `->` and reports the same. Both read as if the signature's last arrow were wrong.

- [ ] **A pacer for incremental marking.** Steps come every 256 KiB of allocation, a constant; the heap overshoots by whatever the program allocates while a cycle marks, which grows with the live set (`churn` at 4,000,000 cells: 359 MB resident). Size each step's allocation from the live set at the cycle's start, so marking ends within a set fraction of the threshold, as Go's pacer does, and the program's share of time while marking follows from that fraction. See [docs/dev/realtime.md](docs/dev/realtime.md) ("Step size").

- [ ] **Library api with prefixed symbols.** `libfunk.h` is internal and unstable, and internal names (`intern`, `lex`, `repr`, `globals`) can collide with a host's. Prefix them, or build a shared library with `-fvisibility=hidden` exporting `funk_*` and the `fk_*` symbols the interpreter's glue needs; install the header and a CMake package. A stable api follows the item below.

- [ ] **Per-instance context.** About 165 file-scope variables (41 in `check.c`, 36 in `rt.c`, 25 in `elab.c`) allow one session per process. Move compiler state into a context passed through the passes, and remove globals where a parameter or a local serves. The runtime's (heap, collector, stack snapshot) are shared with generated C, which pastes `rt.c`: a context pointer there costs every allocation, so measure against the benchmarks before moving them.

- [ ] **Type classes, phase 2.** Kinds and higher-kinded classes (`Functor`, `Foldable`); see [docs/dev/type-classes.md](docs/dev/type-classes.md#later-phases).

## Low

- [ ] `showPrec` for `Float` omits the parentheses around `-0.0` (`Vec -0.0 1.0`); Haskell prints `Vec (-0.0) 1.0`. Needs an `isNegativeZero` check in the prelude instance.
- [ ] `printing`: `show` at `Int` no longer goes through `vfprintf` (-34%). Its `enumFromTo` list is built and kept live, because `mapM_`'s function prints, so fusion refuses it: the collector was about 25% at 20M lines. `++` copies, about 20%. Effect arrows do not change this: `mapM_`'s function acts. Fusion could accept an effectful consumer when every stage before it is pure and total, since only the consumer's effects would then be observable.
- [x] **Typed effects: effect arrows** (`a ->> b`), option E of [docs/dev/typed-effects.md](docs/dev/typed-effects.md).
- [ ] Fusion still uses its own purity analysis. With effects in types it could see through closures, parameters and dictionaries, but elaboration's type copies drop the arrows' effects first.
- [ ] The interpreter spends 26% of its time in `eval`'s prologue and epilogue; split out the hot cases. Interpreter speed is not part of the primary goal.
