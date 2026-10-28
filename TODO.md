# TODO

Always organize according to sections descending priority (Critical, High, Medium, Low). They can include subsections as required for organization.

The next milestone is a plugin with compiled DSP: patterns run live in the interpreter, and a compiled synth renders on the audio thread ([docs/dev/audio.md](docs/dev/audio.md), option A). Direction: [docs/dev/261027-lang-review.md](docs/dev/261027-lang-review.md).

Generated C speed reached its target: geometric mean 1.18x C on the core suite, 1.16x on the Benchmarks Game (2026-10-27). Keep it: check each change with `bench.py ab` ([docs/dev/performance-first.md](docs/dev/performance-first.md)). Compilation speed is now a target as well.

## Critical

- [ ] **Shared-library output with hidden runtime symbols** (`funk build --shared`; [docs/dev/embedding.md](docs/dev/embedding.md) phase 0 item 5). `funk emit-c` pastes the runtime, so a compiled synth linked into a host that also embeds `libfunk` has two runtimes' symbols in one process, and two funk plugins in one DAW may resolve each other's `fk_cons`. `libfunk` also exports unprefixed names (`repr`, `lex`, `global_add`, `compile_chunk`): export only `funk_*` and the `fk_*` symbols the interpreter's glue needs, with `-fvisibility=hidden`; install the header and a CMake package.

- [ ] **Stack overflow in compiled code ends the host process** ([docs/dev/embedding.md](docs/dev/embedding.md) phase 0 item 2). Decide with the item above: recovery by `siglongjmp` is unsafe inside libc or the collector.

- [ ] **Audio ports for the plugin.** `midi-langs`' CLAP framework handles note effects only. Decide whether it grows audio ports or funk gets a plugin of its own ([docs/dev/audio.md](docs/dev/audio.md#open-questions)).

## High

### Compiled DSP ([docs/dev/audio.md](docs/dev/audio.md#recommendation))

- [ ] `lib/dsp.fk` in the export subset, with one voice type: saw, square and sine-table oscillators, ADSR, one-pole and state-variable filters, a fixed number of voices.

- [ ] Wider ring events. A value has 16 bits; note, velocity and channel need 18, and parameters such as cutoff more. Options are listed in `audio.md`.

- [ ] `funk-midi --audio`, with PortAudio or miniaudio, calling the compiled `fk_voice`.

- [ ] Measure the cost per voice and how many fit in 1.33 ms (64 frames at 48 kHz). Log the friction of writing a voice in the export style.

- [ ] `foreign struct`: C arrays inside structs (voice blocks; Core Audio's `AudioBufferList`), and several fields per line, as `data` records accept.

### Compilation speed

- [ ] Set a target, from a baseline. `tests/ffi/sched.fk` builds in 1.03 s at `-O2`, 0.18 s at `-O0`, mostly the pasted runtime's 6,000 lines; foreign-import glue alone takes 87 ms ([docs/dev/audio.md](docs/dev/audio.md#b-a-with-the-synth-recompiled-on-edit)). Measure funk's passes and the C compiler separately over the benchmark suites. Linking a prebuilt runtime, which shared-library output needs anyway, removes the runtime from every compile.

### Friction log ([docs/dev/patterns.md](docs/dev/patterns.md#friction-log))

- [ ] #1: `mapP :: (a -> b) -> Pattern a -> Pattern b` is rejected, and "functions whose effects differ" names neither the field nor the parameter.

- [ ] #3: a field read on a parameter needs a signature; infer the record type when one type has the field.

- [ ] #4: a file that imports a module of its own name reports "import cycle: pattern -> pattern"; say "imports itself".

- [ ] #6: every `funk_eval` input stays compiled, about 16 KB each. Roll back an expression's chunk after it runs.

### Runtime

- [ ] **A pacer for incremental marking.** Steps come every 256 KiB of allocation (`STEP_BYTES`), a constant; the heap overshoots by whatever the program allocates while a cycle marks, which grows with the live set (`churn` at 4,000,000 cells: 359 MB resident). Plugin instances each with a heap multiply it. Size each step's allocation from the live set at the cycle's start, as Go's pacer does. See [docs/dev/realtime.md](docs/dev/realtime.md) ("Step size").

### Visibility

- [ ] **Report which optimizations applied to a function**: fusion, reuse, TRMC, typed worker. Fusion matches prelude names, reuse needs linear use and TRMC a given recursion shape, so a small change can lose one with no sign; `queens` lost 18.7% and `bst` 8.6% during 0.5 and 0.6 ([docs/dev/261027-lang-review.md](docs/dev/261027-lang-review.md), weakness 3).

## Medium

### Embedding

- [ ] **Instances in one runtime** ([docs/dev/embedding.md](docs/dev/embedding.md) phase 1): `funk_instance_new` and `funk_instance_free` over module prefixes, reload within an instance, host calls queued onto funk's thread, compiles behind a mutex. Main unknown: whether the undo log can be split by instance. Acceptance test: a Pd external with two instances, one reloaded while the other plays.

- [ ] `fk_cb_failed` is a plain global written on funk's thread and read on an export's; TSan never observed the write, so it is untested, not shown race-free ([docs/dev/261027-lang-review.md](docs/dev/261027-lang-review.md)).

- [ ] The CLAP tests (`midi-langs`' `test_clap_host`) have not run on Linux.

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

- [ ] **Per-instance context** ([docs/dev/embedding.md](docs/dev/embedding.md) phase 2, deferred). File-scope state allows one session per process. Needed only when instances need independent collector pauses or run on several threads. The runtime's globals are shared with generated C: a context pointer costs every allocation, so measure first.

- [ ] **Type classes, phase 2.** Kinds and higher-kinded classes (`Functor`, `Foldable`, `Monad`); see [docs/dev/type-classes.md](docs/dev/type-classes.md#later-phases). Only if the friction log shows a need: with one `Pattern` type, monomorphic functions suffice ([docs/dev/patterns.md](docs/dev/patterns.md)).

- [ ] `closures` is 1.7x C. Calling an exact-arity closure inline instead of through `fk_apply`, and a partial application's call without its variable-length array, left the time unchanged (-0.6%, reverted). On the loop, funk adds a box, an unbox and a store and reload through the `V *` argument array, which C passes in a register (inferred, not measured). A typed entry point in closures of common shapes (`Int -> Int`) would take the value off memory.

- [ ] Fusion: `enumFromThenTo` as a producer, and nested `concatMap`.

- [ ] Fusion still uses its own purity analysis. With effects in types it could see through closures, parameters and dictionaries, but elaboration's type copies drop the arrows' effects first.

- [ ] `printing`: its `enumFromTo` list is built and kept live, because `mapM_`'s function prints, so fusion refuses it: the collector was about 25% at 20M lines. `++` copies, about 20%. Fusion could accept an effectful consumer when every stage before it is pure and total.

- [ ] The interpreter spends 26% of its time in `eval`'s prologue and epilogue; split out the hot cases. Live patterns run interpreted: the slowest 21.3 ms window takes 0.33 ms ([docs/dev/patterns.md](docs/dev/patterns.md)). Act if that nears the window.

- [ ] `tests/run/reuseown.fk`: `map (scale 2.0) (replicate 3 v)` gets no copy at all, where a spine-only `@reuse0` copy was expected; sound, since nothing is reused, but not explained.
