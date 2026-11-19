# TODO

Always organize according to sections descending priority (Critical, High, Medium, Low). They can include subsections as required for organization.

The plugin with compiled DSP is done as of 0.9.0: patterns run live in the interpreter, and a graph of compiled unit generators, built and rewired at run time, renders on the audio thread ([docs/dev/audio.md](docs/dev/audio.md), option D). The next milestone is roadmap 0.12, Tidal parity of code and runtime, then 0.13, SuperDirt's sound in the plugin ([docs/dev/tidal-comparison.md](docs/dev/tidal-comparison.md)). Direction: [docs/dev/261027-lang-review.md](docs/dev/261027-lang-review.md).

Generated C speed reached its target: geometric mean 1.18x C on the core suite, 1.16x on the Benchmarks Game (2026-10-27). Keep it: check each change with `bench.py ab` ([docs/dev/performance-first.md](docs/dev/performance-first.md)). Generated code speed comes before compilation speed. Compilation speed matters for compiling unit generators while playing, in desktop live coding.

## Critical

- [x] `FUNK_GCSTRESS=1` failed funk-midi's check of marks: a false report, from a scratch register `___chkstk_darwin` saved into an interpreter VLA. Fixed 2026-11-10 ([docs/dev/261110-gcstress-chkstk.md](docs/dev/261110-gcstress-chkstk.md)).

## High

### Test environment

- [x] The scsynth oracles failed falsely when sc3-plugins' classes were installed without its server plugins: SuperDirt built a `dirt_delay` that scsynth refused. They now skip, naming the UGens. Done 2026-11-18 ([docs/dev/261118-reverb-kernel-cost.md](docs/dev/261118-reverb-kernel-cost.md#open)).

- [ ] SuperCollider 3.14.1: the oracles fail against its scsynth, at 0.12.0 too: `plain` errs by 0.116, its onset a frame later and its render 33 frames longer; `p0` by 0.376. funk ports 3.13.0, which passes. Find the scsynth or UGen change, from the 3.13-to-3.14 changelog, then bisect on `plain`. sc3-plugins must match the scsynth version.

- [ ] Without sclang, the oracles are not registered, so `make test` passes without them: CMake's hint missed an install elsewhere for two days. Consider failing when `SCLANG` is set but does not run.

- [ ] Against a develop SuperDirt (`v1.7.4-18`), `render.scd` fails in sclang (`_IdentDict_Put`) and `lib/synthdefs` differs: 15 synths' `speedFreq` defaults. The tests need v1.7.3, whose `.quark` file a develop checkout shares, so a version check cannot tell them apart.

- [ ] Without sc3-plugins at all, SuperDirt uses CombL for `dirt_delay`, but the oracles' non-core cases render funk's `SwitchDelay`, so they would fail. Not reproduced: only checked with the classes hidden.

- [ ] On Linux, with matching plugins, `shape` (2.4e-6), `imp` (7e-4) and `impAccelerate` (2.4e-3) miss the oracle's bound. Find whether libm or scsynth's build differs from the Mac's.

### Tidal parity, roadmap 0.12 ([docs/dev/tidal-comparison.md](docs/dev/tidal-comparison.md))

- [x] L1: `FromString`, so a string literal is a pattern. Done 2026-11-09.

- [x] L2, C1: controls take patterns; `n` and `note` take Tidal's `Note`. Done 2026-11-09.

- [x] Top-level functions of float and string literals generalize, as in Haskell; specialization keeps them unboxed. Done 2026-11-09.

- [x] REPL and host inputs generalize values, as GHCi's prompt does; `:t` prints the generalized type. Done 2026-11-09.

- [x] A redefinition does not change code defined before it, overloaded or not, as in GHCi and Tidal. Done 2026-11-09.

- [x] S9: a SuperDirt target, `funk-midi --dirt`. Done 2026-11-09.

- [x] Listen to `funk-midi --dirt 57120` against a running SuperDirt: it plays, on macOS, 2026-11-09.

- [x] Check `funk-midi --dirt 57120`'s handshake against a real SuperDirt 1.7.3: started first, it connects at the next 2 s retry; started after, in 70 ms. Done 2026-11-10.

- [x] R1: slots `d1`..`d16` and `p "name"`, in funk-midi. Done 2026-11-10 ([docs/dev/stream.md](docs/dev/stream.md)).

- [ ] R1 in the plugin: slots for funk-clap. The 0.13 acceptance's REAPER set plays through `pat` until then ([docs/dev/dirt-acceptance.md](docs/dev/dirt-acceptance.md)).

- [x] R2: the slots' commands, and the listener's OSC ones. Done 2026-11-10 ([docs/dev/stream.md](docs/dev/stream.md#commands)).

- [x] R3, C2: tempo commands and an event's `cps`, in funk-midi. Done 2026-11-10 ([docs/dev/stream.md](docs/dev/stream.md#tempo)).

- [x] R4: transitions, in funk-midi. Done 2026-11-10 ([docs/dev/stream.md](docs/dev/stream.md#transitions)).

- [x] R5: `setF` and its kind, in funk-midi. Done 2026-11-10 ([docs/dev/stream.md](docs/dev/stream.md#state)).

- [x] T2: fractional patterned speed factors. Done 2026-11-10.

- [ ] T3: `Rational` times.

- [ ] F5's `showAll`: needs Tidal's `Show` of `Pattern`, `Arc` and `Value`, and events' contexts, which funk's lack ([docs/dev/tidal-comparison.md](docs/dev/tidal-comparison.md)). Open into 0.13 with T3, R6 and L7.

- [x] L4: a `Fractional` class; `Pattern`, `Note` and `Controls` are instances. Done 2026-11-09.

- [x] L5: `pattern` unqualified in the hosts. Done 2026-11-10.

- [x] L3: `fmap`, `<$>`, `pure`, `<*>`, `<*`, `*>`, `<<*` and `>>=` on patterns. Done 2026-11-10.

- [ ] C3's buses wait for S2; `ringCount` for a name. L8 is an `Enum` class.

- [ ] L7: decide the review item.

- [ ] `Floating` (`pi`, `sin` on patterns); not yet a numbered gap in tidal-comparison.md.

- [ ] `:l` of a file a second time rejects its classes and instances: "class `Big` is already defined", "duplicate instance `Show Sq`". Found 2026-11-11, on HEAD too.

- [ ] The REPL reads one line per input, so a signature and its definition need `:{ :}`; GHCi accepts them on separate lines.

- [ ] `funk repl` overflows the stack on `foldl` over 20M elements (predates 0.12).

- [ ] Fix stale docs: `^name`'s reason in `docs/music.md`; `euclid` in `docs/dev/combinators.md`'s differences.

### SuperDirt's sound, roadmap 0.13 ([docs/dev/dirt-sound.md](docs/dev/dirt-sound.md))

- [x] The event layer, option C: templates, a pool, chains per event, orbits, `dirt` in both hosts. Done 2026-11-11.

- [x] Review decisions 4-6 of dirt-sound.md. Done 2026-11-15: all kept; 6 gains an every-folder form.

- [ ] Decision 6's form that loads every folder of a path, as SuperDirt's `loadSoundFiles`, and the index wrap (`s "bd" # n 13` on 4 files plays `bd:1`), which funk lacks even for named samples. Two design choices first: dirt-sound.md, decision 6.

- [ ] Difference 14 of dirt-sound.md: one real-time heap for the pool, as scsynth's `AllocPool`, instead of fixed size classes (TLSF, or a port of `AllocPool`). A design note first; it supersedes decision 8.

- [x] Plan step 2: `dirt_sample` and `dirt_gate` as SuperDirt's, cubic `BufRd` (S5) and `DirtPan` (S4), checked against scsynth by `lib/dirt-scsynth`. Done 2026-11-11.

- [x] Plan step 4: SuperDirt's core modules and their UGens (S1, S8), each a `lib/dirt-scsynth` case, and size-class pools. Done 2026-11-12.

- [x] `lib/dirt-scsynth` for synths; a case with more than one orbit. Done 2026-11-12.

- [x] Plan step 5: orbits' global effects with `DirtPause` (S2), scsynth's `RGen`, `dirt` at SuperDirt's level. Done 2026-11-12.

- [x] Plan step 6: synths (S3). Done: the UGen library (`lib/scsynth.fk`), `default-synths.scd`'s 5 portable synths. Done 2026-11-12 too: all 32 of `default-synths-extra.scd`'s. Done 2026-11-13: `default-effects-extra.scd`'s 19 effects, and a synth's `cycle` from the event.

- [x] Plan step 7: AIFF (S7), `unit "s"` (S6). Done 2026-11-13.

- [ ] `out_to` and `map_from`, SuperDirt's routing modules, and `dirt_rms`.

- [x] Review decision 13 of dirt-sound.md. Accepted 2026-11-15.

- [x] Decide whether the played graph renders on scsynth's 64-frame grid (difference 4). Decided 2026-11-15: it keeps the host's blocks.

- [x] Review decision 14 of dirt-sound.md. Accepted 2026-11-15; reopened if step 4's CPU per block misses its budget.

- [x] Difference 12 of dirt-sound.md: names on the audio thread no longer use `snprintf`; `kernels/noalloc` and `hosts/clap/noalloc` find no allocation, freeing or locking there. Done 2026-11-15.

- [ ] A direct test that a `g.In` or `g.Bus` read is one node however often a graph reaches it (fixed 2026-11-13). Only `lib/dirt-scsynth`'s `waveloss` case shows it today, and it runs only with sclang.

- [ ] `kernels/templates/thread` fails about once a full `make test` run under load, and passes alone; find why. Not reproduced 2026-11-15: 0 failures in 80 runs, 8 at once beside 8 spinning processes, and 30 more serially beside 10. A late event is not the cause: delaying its send 5 ms still passes, since it starts at once and ends before the next set's. Keep the output (`build/Testing/Temporary/LastTest.log`) when it recurs.

- [x] `playDirt` of `d.superDirt` took about 2.4 s in the interpreter: its walks followed every path of the SynthDefs' shared subtrees. 0.15 s with `g.Ref`, 2026-11-15.

- [ ] Decide whether the session's scsynth probe (diffs a SynthDef's UGen graph against funk's) and CPU bench become tools in `tests/dirt`; they were scratch scripts.

### Compiled DSP graph ([docs/dev/audio.md](docs/dev/audio.md#d-a-graph-of-compiled-unit-generators-built-at-run-time))

- [ ] Node state other than `Float`s, one delay buffer and loaded data ([docs/dev/audio.md](docs/dev/audio.md#d-a-graph-of-compiled-unit-generators-built-at-run-time) item 4).

- [ ] `foreign struct`: C arrays inside structs (voice blocks; Core Audio's `AudioBufferList`), and several fields per line, as `data` records accept.

### From rustel, for roadmap 0.13

Ideas from [tzfm/rustel](https://github.com/tzfm/rustel), a Rust port of Strudel; see [docs/dev/rustel.md](docs/dev/rustel.md). It is AGPL-3.0: take ideas, not code.

- [ ] 0.13 design note: weigh a fixed-order voice record against per-event subgraphs. rustel's voice is a fixed-size record with every effect as an optional field, rendered in one order, plus at most 3 `.FX()` stages (`crates/audio/src/backend.rs:370`). SuperDirt also runs its modules in one order (`DirtEvent.sc:192`, `SuperDirt.sc:174`). Decide first whether parity covers user `addModule` calls and custom SynthDefs; a fixed record cannot.

- [x] A test that the C audio path does not allocate, free or lock: `funkgraph`'s runner, `voices.c` and the plugin's `process()` (`tests/noalloc.c`). Done 2026-11-15; Linux runs it in CI only. `funk-midi --audio`'s Core Audio callback is not covered.

- [ ] Negative controls in the scsynth reference renders: each case also renders a wrong variant that must fail the comparison, so the metric is shown to separate them (`crates/audio/tests/chrome_reference.rs`). Compare samples against a stated error bound, not rustel's peak and RMS.

### Friction log ([docs/dev/patterns.md](docs/dev/patterns.md#friction-log))

- [ ] #4: a file that imports a module of its own name reports "import cycle: pattern -> pattern"; say "imports itself". #11 has the same cause: a file beside the importer shadows a module on the `-M` path, with no warning.

- [ ] #12: a local signature cannot call an enclosing function's parameter; see the log.

### 0.10 acceptance test

- [x] The plugin's simulator mode: scripted steps, rotated or random, evaluated live and logged with each block's `process()` time against its budget ([docs/dev/simulator.md](docs/dev/simulator.md)). Done 2026-11-04.

- [x] Play a full live set in a DAW, with edits, with no missed blocks: passed in REAPER on macOS, 2026-11-07 ([docs/dev/roadmap.md](docs/dev/roadmap.md)).

- [ ] The same on Linux, after roadmap 0.12 (decided 2026-11-09). Log any friction in [docs/dev/patterns.md](docs/dev/patterns.md#friction-log). Needs a person at the DAW.
  - Run `projects/funk-clap/sim-set.fk` in REAPER on Linux, several instances, with Stop, transport and tempo changes by hand. Check the log for `slow`, `busy` and `failed`, and REAPER's own underrun count: the log times this plugin only.

  - If one script per process proves limiting, read the script from each instance's state instead of the environment.

### Correctness

- [ ] `embed/plugin`'s CI failure (run 41272122243) is explained by ASan's fake stack under GCC, now fixed (CHANGELOG), but was not reproduced on Linux. Watch the `address` jobs.

- [ ] `embed/kernels` failed once in a local parallel suite run, 2026-11-02, with its output lost; 12 parallel and 15 repeated runs passed. Keep the output when it recurs.

## Medium

### From rustel

- [ ] Write the scheduling contract of funk-midi's slots and the plugin, and test each clause: events carry their pattern's generation and an old generation is never delivered; onset ids are unique; stop does not wait behind evaluation; stale events expire. rustel's is `crates/scheduler/src/lib.rs:1-20`.

- [ ] R6: `funk-midi --watch FILE` reloads the file on save, so any editor works without a package, as `rustel play --watch`.

- [ ] L7: also refuse an edit whose first window yields an invalid control value, and keep the last pattern playing, as rustel does (`docs/overview.md`, "Refused edits").

- [ ] T3: consider a bounded `Rational`, as rustel's reduced `i128/i128` with a 64-bit path below 2^31 and checked overflow (`crates/fraction/src/lib.rs:1-79`), against Haskell's unbounded one. Record which Tidal patterns would overflow it.

### Friction log ([docs/dev/patterns.md](docs/dev/patterns.md#friction-log))

- [ ] #2: `map p.atom "abc"` reports "expected `[a]`, found `String`"; give a targeted error that suggests `unpack`.

- [ ] #8: a function an export calls cannot return a tuple, since a tuple is a heap value; unboxed multiple returns in exported code would allow it.

- [ ] #9: a loop in an export takes every loop invariant as a parameter (`svfLoop` takes 10); a local loop closing over them, compiled without a closure, would remove them.

### Embedding

- [ ] **Instances in one runtime** ([docs/dev/embedding.md](docs/dev/embedding.md) phase 1, deferred behind sessions): `funk_instance_new` and `funk_instance_free` over module prefixes, reload within an instance, host calls queued onto funk's thread, compiles behind a mutex. Main unknown: whether the undo log can be split by instance. Acceptance test: a Pd external with two instances, one reloaded while the other plays.

- [ ] `fk_cb_failed` is a non-atomic `int`, per session under `FK_SESSIONS` and global otherwise, written on funk's thread and read on an export's; TSan never observed the write, so it is untested, not shown race-free ([docs/dev/261027-lang-review.md](docs/dev/261027-lang-review.md)).

### Real time ([docs/dev/realtime.md](docs/dev/realtime.md))

- [ ] No test proves the stack snapshot's page copy necessary since protection stops below the thread's first frame (`tests/run/gcdeep.fk` passes without it).

- [ ] An export's stack bound is not computed; for targets with small stacks, sum `-fstack-usage` frame sizes along the call graph.

### Correctness

- [ ] `eff_error` takes `$m.Class.Type.method` apart at its first and last `.`. A module's class is `prefix.C`, so `$m.geometry.C.T.m` reports class `geometry` and type `C.T`.

### Speed

- [ ] Generated DSP kernels render 1.05 to 4.6x scsynth's CPU on the same events, samples as synths; `dirt_reverb` and `GVerb` take most of it ([docs/dev/dirt-acceptance.md](docs/dev/dirt-acceptance.md), step 4). On Linux, `dirt_reverb` was 4.2x a C port of itself, and is 0.75x since `cInt` stopped calling libm and the reverb runs one stage at a time (2026-11-18, [docs/dev/261118-reverb-kernel-cost.md](docs/dev/261118-reverb-kernel-cost.md)). On Linux, the 18 patches render 1.62x faster and the synth cases 1.7 to 2.2x. Linux's ratios to scsynth are overstated: its scsynth ran no `dirt_delay`. Next: step 4 again on the Mac, with `ORACLE_CPU=1 ORACLE_REPEAT=60` in `tests/dirt/oracle.py`; then `GVerb`, and `dirt_leslie` at 3.1 us a call, by the note's profile. Time each change: moving `dirt_switch_delay`'s crossfade into parameters made it 36% slower, and was reverted.
  - [ ] On the Mac: `ORACLE_CPU=1 ORACLE_REPEAT=60` against scsynth, for step 4's figures after the 2026-11-18 changes. Linux cannot give them: its scsynth lacks sc3-plugins' server plugins.
  - [ ] Profile `GVerb` (`gvDiff` keeps its lines in slots), then time any change to it.
  - [ ] `dirt_leslie`, 3.1 us a call: its filters, oscillators and delay position are in slots.
  - [ ] `dirt_switch_delay`: run a loop without the crossfade while slot 25 is 0, and the current loop only during a crossfade. Untried.
  - [ ] Time `funk_events_read`'s and `effects`' changes; only their instructions were measured (90% and 77% fewer).
  - [ ] A differential harness for kernels: one kernel from two `funkdsp` builds on the same inputs, output, slots, memory and `done` compared bit for bit, and timed. One was written in the 2026-11-18 session's scratch directory and is not in the repo. Its inputs are in [docs/dev/261118-reverb-kernel-cost.md](docs/dev/261118-reverb-kernel-cost.md).

- [x] `funk_events_read` took 18% of a patch render's instructions, testing each orbit's `used` flag once a frame. It now tests it once a block: 2% (2026-11-18, [docs/dev/261118-reverb-kernel-cost.md](docs/dev/261118-reverb-kernel-cost.md#end-to-end-on-linux)).

- [ ] 64-bit `Int` at `V` boundaries costs at most 0.7% (geometric mean, 6 benchmarks; `queens` 3%). Typed list heads would remove it; [docs/dev/typed-backend.md](docs/dev/typed-backend.md#integers).

- [ ] `bst` is 8.6% slower since element reuse: inserts update the tree in place, and lookups are 27% slower on the layout that leaves (path copying kept the upper levels adjacent). [docs/dev/element-reuse.md](docs/dev/element-reuse.md#phases-2-and-3-result).

- [ ] Element reuse's field reads (`records`' `step p` reads `p.px` before rebuilding `p`), so `step` reuses its particle.

- [ ] Element reuse: a `let`-bound value is never owned, since a lazy `let`'s thunk keeps it; `resolve.c` decides which `let`s are eager after the ownership analysis runs, and an eager one could own its value. A tuple result is a struct only if every tail builds the tuple; a tail calling another such worker (`notAlways` in `tests/run/tupleresult.fk`) keeps the heap tuple.

- [ ] **Benchmarks Game gaps** (`bench/game/`, [bench/README.md](bench/README.md#benchmarks-game-suite)):
  - `knucleotide` is 2.6x C: `IntTable`'s probe reads three slots of an `Array Int` with bounds checks.

  - `fasta` is 2.0x C: each 60-character line is a `[Char]` packed into a `String`.

  - `revcomp` is 2.1x C (through `bench.py`'s pipe; 2.8x from a file): finding newlines and complementing go byte by byte through bounds-checked accesses, and the output is a second buffer, where C works in place in one (288 MB resident against 105).

## Low

### From rustel

- [ ] Count voice steals and report them, as rustel counts the onsets it drops past 512 voices (`crates/audio/src/scalar.rs:254`). `d.kit` steals within its 4 voices a name, where SuperDirt starts a synth per event; the count shows when that difference is audible.

- [ ] Session tapes: log each installed edit with its time, and replay the log, so a live-set acceptance run can be repeated (rustel `docs/sessions.md`).

- [ ] R6, Link: check the licence before an FFI binding. Ableton's Link SDK is GPL-2.0-or-later or commercial; funk is MIT. rustel's behaviour is the model: follow play and stop, join a running session, release MIDI clock in while joined (`docs/hardware.md#link`).

- [ ] Onset placement: check that `d.kit` rounds an onset to the frame as scsynth's `OffsetOut` does. rustel keeps a sub-sample lead (`backend.rs:1137`), which parity with SuperDirt does not want.

### Kernels compiled while playing (desktop live coding)

- [ ] Kernels compiled while playing, outside the plugin: `funk-midi --audio` has no way to evaluate one (`funk_compile_kernels`). Done in the plugin 2026-11-01.

- [ ] Under the hardened runtime, `dlopen` refuses a relative path, so a relative `XDG_CACHE_HOME` would fail every kernel load. Make the cache path absolute. The rest is checked ([docs/dev/plugin.md](docs/dev/plugin.md#hardened-hosts)).

- [ ] **LLVM, as Faust uses it**, only if `$CC` proves too slow for kernels without the runtime: about 60 ms per edit at `-O2`, of which 22 ms is clang's start-up and about 20 ms linking. An IR emitter for the export subset alone would be much smaller than `cgen.c`, since that subset has no allocation, closures or constructors. libtcc compiles faster but does not optimize, and its support for the runtime's `__atomic_*` builtins is unverified.

### funk-midi

- [ ] `funk-midi --audio` estimates a note's frame from its clock; the plugin already gives `pat`'s notes to its voices at exact frames ([docs/dev/plugin.md](docs/dev/plugin.md#notes-midi-and-voices-both)).

- [ ] Run `funk-midi --audio` on Linux, and listen to it.

### Friction log ([docs/dev/patterns.md](docs/dev/patterns.md#friction-log))

- [ ] #6: every `funk_eval` input stays compiled, about 16 KB each. Hosts use `funk_call` per window, so this waits for a host that needs more. A partial rollback was tried and reverted 2026-11-01: it saved 4.8 of 7.4 KB per simple input, and freeing compiler memory risks dangling pointers.

### Other

- [ ] Named arguments, stage 2: presets that keep names, `pluck = adsr .{ ... }`, once patches use presets ([docs/dev/named-args.md](docs/dev/named-args.md)). Also confirm `lib/dsp.fk`'s proposed defaults by ear.

- [ ] DSP phase 3, deferred until a patch needs it: several outputs per kernel, which would halve `d.zita`'s cost ([docs/dev/dsp.md](docs/dev/dsp.md#phase-3-several-outputs-per-kernel)).

- [ ] **Sessions** ([docs/dev/embedding.md](docs/dev/embedding.md#phase-2-sessions)): glue shared by path binds the last session's `funk_provide`d addresses.

- [ ] `--opt-report` lists the workers of the program's own functions only: not anonymous lambdas, nor prelude functions copied for the program's calls (`foldr@{0=insert}`). Add them if a slowdown hides there.

- [ ] **Type classes, phase 2.** Kinds and higher-kinded classes (`Functor`, `Foldable`, `Monad`); see [docs/dev/type-classes.md](docs/dev/type-classes.md#later-phases). Only if the friction log shows a need: with one `Pattern` type, monomorphic functions suffice ([docs/dev/patterns.md](docs/dev/patterns.md)).

- [ ] `closures` is 1.7x C. Calling an exact-arity closure inline instead of through `fk_apply`, and a partial application's call without its variable-length array, left the time unchanged (-0.6%, reverted). On the loop, funk adds a box, an unbox and a store and reload through the `V *` argument array, which C passes in a register (inferred, not measured). A typed entry point in closures of common shapes (`Int -> Int`) would take the value off memory.

- [ ] Fusion: `enumFromThenTo` as a producer, and nested `concatMap`.

- [ ] Fusion still uses its own purity analysis. With effects in types it could see through closures, parameters and dictionaries, but elaboration's type copies drop the arrows' effects first.

- [ ] `printing`: its `enumFromTo` list is built and kept live, because `mapM_`'s function prints, so fusion refuses it: the collector was about 25% at 20M lines. `++` copies, about 20%. Fusion could accept an effectful consumer when every stage before it is pure and total.

- [ ] The interpreter spends 26% of its time in `eval`'s prologue and epilogue; split out the hot cases. Live patterns run interpreted: the slowest 21.3 ms window takes 0.33 ms ([docs/dev/patterns.md](docs/dev/patterns.md)). Act if that nears the window.

- [ ] `tests/run/reuseown.fk`: `map (scale 2.0) (replicate 3 v)` gets no copy at all, where a spine-only `@reuse0` copy was expected; sound, since nothing is reused, but not explained.
