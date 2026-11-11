# Roadmap

A provisional plan, ordered by value per unit of work. The concrete items for the next release are in [TODO.md](../../TODO.md). Version numbers are targets, not commitments.

## 0.1 -- done

Core language, HM inference with `Num`, interpreter, REPL, C backend, conservative GC. See [CHANGELOG.md](../../CHANGELOG.md).

## 0.2 -- done: Haskell semantics and performance

Released 2026-10-25; see [CHANGELOG.md](../../CHANGELOG.md). It covers what the plan had split into two releases, 0.2 and 0.3.

Haskell semantics:

- Lazy `let` and `where` value bindings.

- Compile-time warning for non-exhaustive patterns.

- `infixl`, `infixr` and `infix` declarations.

- Records with type-scoped fields and dot access ([records.md](records.md)).

- Type classes, phase 1: `Eq`, `Ord`, `Show`, `Num`, user classes, deriving ([type-classes.md](type-classes.md)).

- `do` blocks with `<-` and `let`.

- A benchmark suite against C (`make bench`).

- Type errors at the offending sub-expression, not the binding.

Performance, the primary goal:

Generated C speed outranks every other goal; see [performance-first.md](performance-first.md). The metric is the geometric mean of compiled time over C time on the benchmark suite: 10.0x on an M1 (2.6-62x). The target is 2x. Measure with `make bench` and `bench/bench.py report`.

1. ~~Inline primitives at known types in generated C.~~

2. ~~Collector hot paths: reject tagged words before searching, stop zeroing dead objects.~~

3. ~~Stack overflow through a guard page, removing the per-call check.~~

4. ~~Drop unreachable code, prelude instances included.~~

   Steps 1-4 took the geometric mean from 10.3x to 6.6x; collector marking fixes then took it to 4.9x, typed locals to 4.0x, eager `let` to 2.6x, specialization on function arguments to 2.4x, TRMC to 2.06x, lifting and specializing lambdas that capture locals to 1.93x, packed strings to 1.79x, reuse of list cells to 1.68x, list fusion to 1.52x, and typed workers to 1.37x; 64-bit `Int` in both backends then cost 5.4%, to 1.48x; typed copies of higher-order functions and bump allocation brought it to 1.35x.

5. ~~Typed backend: C scalars in monomorphic code, workers with typed parameters, unboxed scalar fields ([typed-backend.md](typed-backend.md)).~~

6. ~~Memory ([memory.md](memory.md)): tail recursion modulo cons first, then a nursery or Perceus reference counting, chosen from a profile.~~ TRMC, in-place reuse of list cells and list fusion; a nursery and Perceus were measured and rejected.

7. ~~Evaluate pure, total, cheap `let` bindings eagerly.~~ Geometric mean 4.0x to 2.6x.

Interpreter speed (splitting `eval`, 26% in its prologue) is not part of this goal.

## 0.3 -- done: typed effects

Released 2026-10-26; see [CHANGELOG.md](../../CHANGELOG.md).

- Effect arrows, option E of [typed-effects.md](typed-effects.md): `a ->> b`, inferred and checked; effectful bindings are `do` statements; `trace`.

- Prelude functions, methods, types and constructors are reserved.

## 0.4 -- done: foreign functions and real time

Released 2026-10-27; see [CHANGELOG.md](../../CHANGELOG.md).

- A foreign function interface ([ffi.md](ffi.md)): libm, sqlite3 and libremidi; C types, structs, pointers, callbacks.

- Real time ([realtime.md](realtime.md)): incremental collection with pauses under 0.5 ms at any heap size and stack depth, a lock-free event ring, allocation-free exports for audio threads, `Array`, `Ring` and atomics; a PortAudio synth.

## 0.5 -- done: the Benchmarks Game and ownership

Released 2026-10-27; see [CHANGELOG.md](../../CHANGELOG.md).

- The Benchmarks Game's 10 tasks against its single-threaded C ([bench/README.md](../../bench/README.md#benchmarks-game-suite)): geometric mean 1.16x C; the core suite 1.18x.

- Reuse of cells by ownership: list elements, tuple components and any allocation of a matched cell's size ([element-reuse.md](element-reuse.md)); tuple results as C structs; n-body from 4.0x to 1.2x C.

- Bitwise operations, `IntTable`, and bulk `String` and `Array` input and output.

## 0.6 -- done: modules

Released 2026-10-27; see [CHANGELOG.md](../../CHANGELOG.md).

- Modules ([modules.md](modules.md)): files found by path and `-M`, qualified imports, import and export lists, abstract types, `import` at the REPL prompt, and Haskell's base imports.

- CI on Linux and macOS, plain and under UBSan.

## 0.7 -- done: embedding, patterns and audio

Released 2026-10-30; see [CHANGELOG.md](../../CHANGELOG.md).

- Embedding ([embedding.md](embedding.md), phase 0): `libfunk.h`'s output, interrupt, memory and provide calls; only `funk_*` exported; foreign imports inside a plugin; `cmake --install` with a CMake package.

- Patterns ([patterns.md](patterns.md)): `lib/pattern.fk` and `lib/midi.fk`, and `funk-midi`, a REPL that plays `pat` live.

- Audio ([audio.md](audio.md)): kernels from `funk emit-c --kernels`, the `funkgraph` runner, `lib/graph.fk` and `lib/dsp.fk`, voices per MIDI channel.

- The CLAP plugin ([plugin.md](plugin.md)): moved from `midi-langs`, with a stereo output, note input from the DAW and linked glue, so it needs no C compiler.

## 0.8 -- done: sessions

Released 2026-10-30; see [CHANGELOG.md](../../CHANGELOG.md).

- Sessions ([embedding.md](embedding.md), phase 2): every `libfunk.h` call takes a `funk *`; any number of sessions per process, each on its own thread, with its memory freed by `funk_free`.

- The CLAP plugin ([plugin.md](plugin.md#instances)): any number of instances in one process.

- AddressSanitizer: the suite runs clean under it, and CI runs it on Linux and macOS.

## 0.9 -- done: live DSP -- done

Released 2026-11-01; see [CHANGELOG.md](../../CHANGELOG.md).

- Kernels compiled while playing, in the CLAP plugin ([music.md](../music.md)): `funk_compile_kernels`, with `$CC`.

- DSP ([dsp.md](dsp.md)): `d.zita` with smoothed parameters, wavetables and sample playback; equal subtrees of a graph build once.

- Named arguments ([named-args.md](named-args.md)): signatures name parameters and give defaults, and `.{ }` fills them.

## 0.10 -- Stabilization

- ~~Failures: explain or bound the ffi/sched/tsan-compiled hang and the embed/plugin stack overflow.~~ Both explained and fixed ([CHANGELOG.md](../../CHANGELOG.md)).

- ~~Coverage: add a TSan job to CI, and check kernel compiles in a hardened host.~~ See [plugin.md](plugin.md#hardened-hosts).

- ~~Memory: add the pacer, since heap overshoot multiplies with each plugin instance.~~ The overshoot proved small; what multiplied was the 128 MiB threshold floor, now 16 MiB in the plugin ([realtime.md](realtime.md#pacer)).

- ~~Visibility: add the optimization report.~~ `funk emit-c --opt-report`.

- ~~Acceptance test: play a full live set in a DAW, with edits, with no missed blocks. Log any friction it turns up.~~ Passed in REAPER on macOS, 2026-11-07: the simulator's set ran 48 steps over 6.4 min with no block over budget, and the longest took 15% of its budget. Friction: [simulator.md](simulator.md#in-reaper).

## 0.11 -- tidal expressive patterns

Get close to Tidal's expressiveness in funk, through library work first and language features only where the library needs them. Evidence and open proposals: [tidal.md](tidal.md).

The items struck through below shipped in release 0.10.0, with 0.10's ([CHANGELOG.md](../../CHANGELOG.md)). Milestone numbers here are not release numbers.

Library, in `lib/pattern.fk`:

- ~~**Pattern application and joins:** monomorphic `apP` and `joinP`, with the left-structure and right-structure forms of Tidal's `<*` and `*>`, and `squeezeJoin`'s behaviour.~~ Functions only, no operators ([patterns.md](patterns.md#implementation)).

- ~~**Patterned arguments:** combinators accept patterns as parameters, such as `fast (p "2 4")`, as Tidal's `patternify` gives.~~ `fast`, `slow` and `shift`, with ratios through `_fastR` ([patterns.md](patterns.md#implementation)).

- ~~**Mini-notation:** an explicit parse call, `p "bd*2 sn"` ([patterns.md](patterns.md#literals)). Overloaded string literals are not planned.~~ `p.mini`, for `Int` values ([music.md](../music.md#mini-notation)).

- ~~**Control patterns:** named parameters on events, and a `#` that combines them, as Tidal's `ValueMap` and `Unionable` do. This settles the event payload question in [patterns.md](patterns.md#open).~~ `lib/control.fk`: Tidal's 257 controls and its operators, played as MIDI, on the voices and on `d.kit` ([delivery.md](delivery.md)).

- **Combinators:** Tidal's transformations, such as `rev`, `every` and `degradeBy`. All of `Core.hs`, `UI.hs` and `Control.hs` that is not commented out, with continuous patterns, and queries that carry controls, for `cF` and `trigger`, from OSC, MIDI CC and plugin parameters ([controls-in.md](controls-in.md)) ([combinators.md](combinators.md#status)).

Host and time decisions these depend on:

- ~~**Delivery:** `sampleEvents`, `pushOnsets` and `writeEvents` carry an `Int` value, and `pushOnsets` only 16 bits. Control patterns need a path to the voices and the graph.~~ `writeControls`' records, decided and built in three steps ([delivery.md](delivery.md)).

- ~~**Time:** ticks divide exactly by factors of 720720. Tidal's `Rational` allows `fast 1.5` and `0.25 ~> p`. Decide whether fractional factors round, or time changes.~~ They round: `_fastR n d` is `_fast n (_slow d p)`, and time stays in ticks. `~>` is a combinator still to write.

Language features, each only if the library work shows the need:

- `type` synonyms, for names such as `ControlPattern`.

- As-patterns.

- Higher-kinded classes, if a second pattern-like type appears or Tidal's `<$>` and `<*>` names prove necessary. Design risk: effects in class methods ([tidal.md](tidal.md#kinds-and-higher-kinded-classes)).

Acceptance test:

- ~~A set of real Tidal patches ported to funk, with each remaining difference listed.~~ The 22 of Tidal's `old/examples/cycles.tidal`: every event matches Tidal 1.10.2's, 1604 of 6456 by rounding to ticks ([patch-port.md](patch-port.md)).

- ~~Patterned arguments and control patterns keep the slowest query well inside its window, measured in the interpreter.~~ The slowest 10 ms window took 2.0 ms, and every mean was at most 0.8 ms ([patch-port.md](patch-port.md#timing)).

## 0.12 -- Tidal parity: code and runtime

Tidal code runs in funk as written, and plays on SuperDirt. Each item names its entry in [tidal-comparison.md](tidal-comparison.md); a difference that stays needs a reason recorded there.

In order:

1. **String literals as patterns (L1, L2, C1):** a `FromString` class, as `FromFloat`, with a `String` default; controls and patterned arguments take patterns. This closes `sound` of a pattern, `pan sine`, `fast "<2 4>"` and `"<0 0.25>" <~ p`.
2. **A SuperDirt target (S9):** `/dirt/play` over OSC, as Tidal's `superdirtTarget`, from `funk-midi`. Done 2026-11-09 ([dirt.md](dirt.md)).
3. **Slots and commands (R1-R5):** `d1`..`d16` and `p "name"`, `hush`, `once`, `solo`, `mute` and the rest, `setcps` and `setbpm` with Tidal's default of 0.575, an event's `cps`, transitions, and `setF` and its kind. Done 2026-11-10 in funk-midi ([stream.md](stream.md)).
4. **Time (T1-T3):** 46126080 ticks per cycle; fractional patterned speed factors.
5. **Library (F1-F8, L3-L5, L8, M1-M3, C3):** `Fractional`, `RealFrac`, `Bool` mini-notation, scales and `Simple.hs` (done), an `Enum` class, `Time.hs`, the exported `Pattern.hs` functions, `drawLine`, steps, `^name`, `Params.hs`' generated families, monomorphic `<$>`, `<*>`, `<*`, `*>` and `<<*`, and an unqualified `pattern` import.
6. **Review items (L7, F10, F11):** decide each.

Acceptance test: the 22 patches of [patch-port.md](patch-port.md) in their Tidal 1.10 form, run unchanged but for the differences kept in [tidal-comparison.md](tidal-comparison.md), with every event matching Tidal's; and a live set on `d1`..`d4` with `hush`, `xfade` and `setcps`, played through SuperDirt.

## 0.13 -- SuperDirt sound in the plugin

`d.kit` and the voices sound as SuperDirt does, for hosts without SuperCollider (S1-S8, S10 in [tidal-comparison.md](tidal-comparison.md)): per-event effects, orbits with their global effects, the synths, the pan law, cubic interpolation and AIFF. Ports of SuperCollider's UGens from its C++, checked against scsynth's non-real-time renders. Needs a design note first: per-event chains in a fixed graph, and node state other than `Float`s ([audio.md](audio.md) item 4). Open: scsynth's RNG for random UGens, and `float32` against a stated error bound.

## 0.14+ -- possible language growth

Larger changes, each needing its own design note in `docs/dev/` first.

- Haskell syntax guides funk's syntax and does not constrain it. [haskell-compat.md](haskell-compat.md) lists what stops Haskell source from running; close a gap only where funk programs need it.

- Type classes phase 2: kinds and higher-kinded classes (`Functor`, `Foldable`), if 0.11 did not need them.

- Phase 3: existential types, for lists of mixed instances.

- List comprehensions.

- `type` synonyms and as-patterns, if 0.11 did not need them.

## Open questions

- Should the interpreter be kept once compile-and-load is fast enough? Two backends double the cost of every semantic change.
