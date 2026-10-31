# TODO

Always organize according to sections descending priority (Critical, High, Medium, Low). They can include subsections as required for organization.

The next milestone is a plugin with compiled DSP: patterns run live in the interpreter, and a graph of compiled unit generators, built and rewired at run time, renders on the audio thread ([docs/dev/audio.md](docs/dev/audio.md), option D). Direction: [docs/dev/261027-lang-review.md](docs/dev/261027-lang-review.md).

Generated C speed reached its target: geometric mean 1.18x C on the core suite, 1.16x on the Benchmarks Game (2026-10-27). Keep it: check each change with `bench.py ab` ([docs/dev/performance-first.md](docs/dev/performance-first.md)). Generated code speed comes before compilation speed. Compilation speed matters for compiling unit generators while playing, in desktop live coding.

## Critical

## High

- [ ] **Stack overflow in compiled code ends the host process** ([docs/dev/embedding.md](docs/dev/embedding.md) phase 0 item 2). Recovery by `siglongjmp` is unsafe inside libc or the collector. Not due for the plugin: it runs interpreted code and kernels, which cannot recurse but by a tail call of themselves ([docs/dev/plugin.md](docs/dev/plugin.md)). gh:2

### Compiled DSP graph ([docs/dev/audio.md](docs/dev/audio.md#d-a-graph-of-compiled-unit-generators-built-at-run-time))

- [ ] Decide on fused multiply-add: an explicit `fma`, or contraction in kernels only. Clang's default contraction makes the C one-pole and filter 1.4x faster, but changes results against the interpreter.

- [ ] Several outputs per kernel, for kernels whose channels share state, such as a stereo reverb.

- [ ] Node state other than `Float`s ([docs/dev/audio.md](docs/dev/audio.md#d-a-graph-of-compiled-unit-generators-built-at-run-time) item 4).

- [ ] funk's `Ring Int` still carries 16-bit values (`pattern.pushOnsets`); only a hand-written export reading it, option A, needs more.

- [ ] Notes from `pat` to the graph: voices, as controls, scheduled against the player's frame count rather than the host's clock.

- [ ] Run `funk-midi --audio` on Linux, and listen to it.

- [ ] `foreign struct`: C arrays inside structs (voice blocks; Core Audio's `AudioBufferList`), and several fields per line, as `data` records accept.

### DSP ([docs/dev/dsp.md](docs/dev/dsp.md#proposed-roadmap))

Phase 2, delay memory:

- [ ] Delay memory in node state, kept across a graph swap without copying it on the audio thread. This is the first use of "Node state other than `Float`s", under Compiled DSP graph.

- [ ] Delay, comb and allpass.

- [ ] Interpolated delay reads, then chorus and flanger.

- [ ] A mono reverb from combs and allpasses, one per channel.

Phases 3 and 4 are under Medium.

### Friction log ([docs/dev/patterns.md](docs/dev/patterns.md#friction-log))

- [ ] #3: a field read on a parameter needs a signature; infer the record type when one type has the field.

- [ ] #4: a file that imports a module of its own name reports "import cycle: pattern -> pattern"; say "imports itself".

- [ ] #6: every `funk_eval` input stays compiled, about 16 KB each. Roll back an expression's chunk after it runs.

- [ ] #12: a local signature cannot call an enclosing function's parameter; see the log.

### Runtime

- [ ] **A pacer for incremental marking.** Steps come every 256 KiB of allocation (`STEP_BYTES`), a constant; the heap overshoots by whatever the program allocates while a cycle marks, which grows with the live set (`churn` at 4,000,000 cells: 359 MB resident). Plugin instances each with a heap multiply it. Size each step's allocation from the live set at the cycle's start, as Go's pacer does. See [docs/dev/realtime.md](docs/dev/realtime.md) ("Step size").

### Visibility

- [x] Where `projects/tree-sitter-funk` lives: in this repository, its sources only. `src/parser.c` and the other generated files are not committed; `make tree-sitter` makes them, its README installs the grammar in Helix and Neovim, and the plugin colours its editor where it has run. A separate repository with `parser.c` committed, for Helix to fetch, was the alternative.

- [ ] **Report which optimizations applied to a function**: fusion, reuse, TRMC, typed worker. Fusion matches prelude names, reuse needs linear use and TRMC a given recursion shape, so a small change can lose one with no sign; `queens` lost 18.7% and `bst` 8.6% during 0.5 and 0.6 ([docs/dev/261027-lang-review.md](docs/dev/261027-lang-review.md), weakness 3).

## Medium

### DSP, after phase 2 ([docs/dev/dsp.md](docs/dev/dsp.md#proposed-roadmap))

Phase 3, several outputs per kernel, only if the per-channel reverb is not good enough:

- [ ] Several outputs, listed under Compiled DSP graph.

- [ ] A stereo reverb.

Phase 4, loaded data:

- [ ] Shared read-only buffers, kept alive until the last graph that reads them is freed. Use a read-only type over phase 2's allocation and freeing.

- [ ] A wavetable oscillator.

- [ ] Reading audio files, and a sampler.

### Embedding

- [ ] **Instances in one runtime** ([docs/dev/embedding.md](docs/dev/embedding.md) phase 1, deferred behind sessions): `funk_instance_new` and `funk_instance_free` over module prefixes, reload within an instance, host calls queued onto funk's thread, compiles behind a mutex. Main unknown: whether the undo log can be split by instance. Acceptance test: a Pd external with two instances, one reloaded while the other plays.

- [ ] `fk_cb_failed` is a plain global written on funk's thread and read on an export's; TSan never observed the write, so it is untested, not shown race-free ([docs/dev/261027-lang-review.md](docs/dev/261027-lang-review.md)).

- [ ] The CLAP tests (`tests/hosts/clap`, `-DFUNK_CLAP=ON`) have not run on Linux, and CI does not build the plugin.

### Real time ([docs/dev/realtime.md](docs/dev/realtime.md))

- [ ] No test proves the stack snapshot's page copy necessary since protection stops below the thread's first frame (`tests/run/gcdeep.fk` passes without it).

- [ ] An export's stack bound is not computed; for targets with small stacks, sum `-fstack-usage` frame sizes along the call graph.

### Correctness

- [ ] `eff_error` takes `$m.Class.Type.method` apart at its first and last `.`. A module's class is `prefix.C`, so `$m.geometry.C.T.m` reports class `geometry` and type `C.T`.

### Speed

- [ ] 64-bit `Int` at `V` boundaries costs at most 0.7% (geometric mean, 6 benchmarks; `queens` 3%). Typed list heads would remove it; [docs/dev/typed-backend.md](docs/dev/typed-backend.md#integers).

- [ ] `bst` is 8.6% slower since element reuse: inserts update the tree in place, and lookups are 27% slower on the layout that leaves (path copying kept the upper levels adjacent). [docs/dev/element-reuse.md](docs/dev/element-reuse.md#phases-2-and-3-result).

- [ ] Element reuse's field reads (`records`' `step p` reads `p.px` before rebuilding `p`), so `step` reuses its particle.

- [ ] Element reuse: a `let`-bound value is never owned, since a lazy `let`'s thunk keeps it; `resolve.c` decides which `let`s are eager after the ownership analysis runs, and an eager one could own its value. A tuple result is a struct only if every tail builds the tuple; a tail calling another such worker (`notAlways` in `tests/run/tupleresult.fk`) keeps the heap tuple.

- [ ] **Benchmarks Game gaps** (`bench/game/`, [bench/README.md](bench/README.md#benchmarks-game-suite)):
  - `knucleotide` is 2.6x C: `IntTable`'s probe reads three slots of an `Array Int` with bounds checks.

  - `fasta` is 2.0x C: each 60-character line is a `[Char]` packed into a `String`.

  - `revcomp` is 2.1x C (through `bench.py`'s pipe; 2.8x from a file): finding newlines and complementing go byte by byte through bounds-checked accesses, and the output is a second buffer, where C works in place in one (288 MB resident against 105).

## Low

### Kernels compiled while playing (desktop live coding)

- [ ] **Compile and load a kernel on demand** with `$CC` and `dlopen`, as foreign-import glue does (`ffi.c`). A kernel that fails the allocation check or the C compiler leaves the old graph playing. Set an edit-latency target from the measured compile time; the optimization level trades it against the cost per node.

- [ ] **LLVM, as Faust uses it**, only if `$CC` proves too slow for kernels without the runtime: about 60 ms per edit at `-O2`, of which 22 ms is clang's start-up and about 20 ms linking. An IR emitter for the export subset alone would be much smaller than `cgen.c`, since that subset has no allocation, closures or constructors. libtcc compiles faster but does not optimize, and its support for the runtime's `__atomic_*` builtins is unverified.

### Other

- [ ] **Sessions** ([docs/dev/embedding.md](docs/dev/embedding.md#phase-2-sessions)): a `funk *` on every call, state passed explicitly, a thread per session. Measured 2026-10-30: a thread-local pointer costs the interpreter 27%, an explicit one nothing ([docs/dev/261030-session-measurements.md](docs/dev/261030-session-measurements.md)). Done 2026-10-30: any number of sessions, up to 256, side by side; tests `embed/sessions`, `embed/globals`. Left: glue shared by path binds the last session's `funk_provide`d addresses; a TSan job in CI.

- [ ] **Type classes, phase 2.** Kinds and higher-kinded classes (`Functor`, `Foldable`, `Monad`); see [docs/dev/type-classes.md](docs/dev/type-classes.md#later-phases). Only if the friction log shows a need: with one `Pattern` type, monomorphic functions suffice ([docs/dev/patterns.md](docs/dev/patterns.md)).

- [ ] `closures` is 1.7x C. Calling an exact-arity closure inline instead of through `fk_apply`, and a partial application's call without its variable-length array, left the time unchanged (-0.6%, reverted). On the loop, funk adds a box, an unbox and a store and reload through the `V *` argument array, which C passes in a register (inferred, not measured). A typed entry point in closures of common shapes (`Int -> Int`) would take the value off memory.

- [ ] Fusion: `enumFromThenTo` as a producer, and nested `concatMap`.

- [ ] Fusion still uses its own purity analysis. With effects in types it could see through closures, parameters and dictionaries, but elaboration's type copies drop the arrows' effects first.

- [ ] `printing`: its `enumFromTo` list is built and kept live, because `mapM_`'s function prints, so fusion refuses it: the collector was about 25% at 20M lines. `++` copies, about 20%. Fusion could accept an effectful consumer when every stage before it is pure and total.

- [ ] The interpreter spends 26% of its time in `eval`'s prologue and epilogue; split out the hot cases. Live patterns run interpreted: the slowest 21.3 ms window takes 0.33 ms ([docs/dev/patterns.md](docs/dev/patterns.md)). Act if that nears the window.

- [ ] `tests/run/reuseown.fk`: `map (scale 2.0) (replicate 3 v)` gets no copy at all, where a spine-only `@reuse0` copy was expected; sound, since nothing is reused, but not explained.
