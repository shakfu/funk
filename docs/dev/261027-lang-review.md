# Language review at 0.6.0

Date: 2026-10-27. A review of funk after the 0.6.0 release. Claims rest on this session's measurements and code reading; where a judgment is opinion, it says so.

**Decision.** funk's direction is a real-time language with Haskell syntax, not a strict Haskell.

## Strengths

- **Speed.** Compiled code runs at 1.18x C on the core suite and 1.16x C on the Benchmarks Game, from pure code; n-body is 1.2x with no mutation ([bench/README.md](../../bench/README.md)). The core suite's C does the same work as the funk programs (lists, recursion, closures); it is not idiomatic C with arrays.

- **Real time.** Collector pauses stay under 0.5 ms at any heap size and stack depth. Exports for audio threads do not allocate. With foreign imports of libremidi and PortAudio, this is a coherent and unusual combination ([realtime.md](realtime.md)).

- **Effects without monads.** `->>` is light and checked ([typed-effects.md](typed-effects.md)). The 0.6.0 fix, where an effectful function could not call a parameter declared pure, was a rough edge, not a flaw in the design.

- **Whole-program optimization held as features were added.** Modules did not break fusion, specialization or cell reuse across files ([modules.md](modules.md)).

- **Discipline.** 386 tests over two backends, UBSan, GCC and CI; interleaved A/B timing for performance decisions; design notes that record rejected alternatives. Bugs surface quickly: the PCRE JIT gap, the fixity order of imported operators, the privacy leak through unexported types.

## Weaknesses

1. **A split identity (opinion).** "Haskell syntax, ML semantics" is clear, but the departures accumulate: packed `String`, strictness with lazy `let`, `->>`, `f.g` as field access, lowercase modules, reserved prelude names. Each is justified alone. Together, Haskell code does not port, while the syntax suggests it should. The base-import compatibility of 0.6.0 is cosmetic: its first test program failed on `map C.ord "ab"`, which needs `unpack`.

2. **Abstraction power now limits more than speed does.** There are no higher-kinded classes (`Functor`, `Foldable`, `Monad`), no `newtype`, no real `Integral` or `Fractional`, no existential types, and at most 64 classes in a session. Modules divide code into files, but reusable libraries run into these gaps.

3. **Performance comes from pattern recognition, and has cliffs.** Fusion recognizes the prelude's functions by name; reuse needs linear use; TRMC needs a given recursion shape. During 0.5 and 0.6, `queens` lost 18.7% and `bst` 8.6% from changes elsewhere. A user cannot see why their code is slow. This is the largest risk to the speed claim on real programs.

4. **The compiler is getting hard to change.** About 12,000 lines, concentrated in `check.c`, `elab.c` and `parse.c`; about 165 file-scope globals; two backends, so every semantic change is made twice. The roadmap's open question, whether to keep the interpreter, grows more expensive the longer it waits.

5. **Little ecosystem.** No standard library beyond the prelude, no package manager. Reserved prelude names mean that growing the prelude can break programs.

## The question this review raised

Who is funk for? The evidence pointed to a niche: strict, fast, real-time functional code for audio and MIDI, with C close at hand. Haskell compatibility pulls toward a different, larger target, where funk would always trail GHC. The decision above settles it for the niche.

Consequences for planning (to be decided against the friction log below):

- The 0.7 items in [roadmap.md](roadmap.md), higher-kinded classes and more Haskell compatibility, serve compatibility more than the niche. Higher-kinded classes may still pay: `Foldable` over event streams is useful.

- Missing from the roadmap: a small library for the domain (ring buffers, MIDI, DSP), and a way to show which optimizations applied to a function, so that users see weakness 3 before they hit it.

## Next step

Write two or three real programs in the niche, such as a MIDI sequencer or a DSP chain, and log each friction point. Choose between language features and domain work from that log rather than by taste.

## Addendum: target users and hosts

Target users: people who live-code in Haskell (Tidal) or in Tidal's style (Strudel). funk would be a small, fast, embeddable engine for them. Target hosts: [midi-langs](https://github.com/shakfu/midi-langs), [psnd](https://github.com/shakfu/psnd), Max/MSP, Pure Data, VST3/AU/CLAP plugins, and Eurorack ([ssp](https://github.com/shakfu/ssp)).

### Syntax

Keep Haskell syntax. A move to ML syntax was considered and rejected:

- Tidal users already write Haskell, and Strudel users write its idioms.

- A syntax change addresses only weakness 1. It costs a rewrite of `parse.c`, the tests and the examples.

- PureScript and Elm are strict, with Haskell-like syntax, and are not read as Haskell ports.

- `midi-langs` already has `mhs-midi` on MicroHs, which is compatible Haskell. funk's distinction is speed and real time, not compatibility.

Weakness 1 comes from the compatibility promise, so the fix is positioning:

1. Demote the 0.6.0 base-import shims. They invite ports that then fail.

2. Present funk as "Haskell-family syntax, strict semantics". Turn [haskell-compat.md](haskell-compat.md) into a user-facing page.

3. Give targeted errors for known mismatches, e.g. "String is packed; use `unpack`" for `map C.ord "ab"`.

### Correction: higher-kinded classes serve the niche

A Tidal `Pattern` is a function from a time span to events, not a lazy stream, so strictness costs Tidal-style code little. Its combinators rest on `Functor` and `Applicative` (`fmap`, `|+|`, `#`) and on user-defined operators. The 0.7 higher-kinded classes are niche work, not compatibility work. This is a reading of Tidal's design, not a measurement.

### Constraints the hosts expose

From [libfunk.h](../../projects/funk/src/libfunk.h) and [architecture.md](architecture.md):

| Constraint today | Hosts it blocks |
|-|-|
| One session per process, one thread; compiler tables and heap are global | Plugins load many instances per process; Max and Pd create many objects per patch |
| All calls run inside `funk_main`, on a stack the runtime owns | Plugins and externals run on threads the host owns |
| Fast code needs `$CC` at run time (`funk_build`, foreign-import glue) | Plugins and Eurorack ship without a compiler; iOS forbids run-time code generation |
| The interpreter is the only path without `$CC`, and it is slow | Live coding needs sub-second reload |
| Errors go to stderr | Max, Pd and plugin hosts have their own consoles, or none |
| No WASM target | Strudel users work in the browser |

This answers the roadmap's question about keeping the interpreter. Live coding needs reload without `$CC`; plugins need speed without `$CC`. Options:

1. **Keep the interpreter, fast enough for control rate** (patterns, MIDI). Audio-rate DSP is compiled ahead of time. This matches Tidal's split between patterns and SuperDirt. Cheapest.

2. **Embed libtcc as the only compiler.** One backend, millisecond compiles, no external `$CC`. Not allowed on iOS. Code is likely 2-5x slower than `-O2` (unmeasured).

3. **Replace the interpreter with a bytecode VM.** Portable and WASM-friendly, with no JIT restrictions. Most new work.

### Open questions

- Is funk the pattern language, the DSP language, or both? Both doubles the reload problem.

- Which host first? A Pd or Max external is the cheapest test of multiple instances; CLAP is the strictest test; `psnd` is the closest to live coding.

- Does the SSP fit? `$CC` may exist if it runs ARM Linux. The 128 MiB heap floor recorded in [memory.md](memory.md) may not fit; current value unchecked.

### Revised next step

Write one friction-log program per constraint:

1. A Tidal-style pattern engine in `psnd`: reload speed.

2. A Pd external with two instances: global state.

3. A CLAP note effect: host-owned thread, no `$CC`.

The multi-session refactor is scoped in [embedding.md](embedding.md): embedding basics and instances in one runtime first, separate sessions deferred.

## Addendum: review comments, 2026-10-28

### Inconsistencies in this note

- The first bullet of "Consequences for planning" says the 0.7 higher-kinded classes serve compatibility. "Correction" reverses it. Read the first bullet as superseded.

- The domain library and the per-function optimization report are absent from "Revised next step". Their status is open: deferred or dropped.

### Tidal requirements the Correction omits

From Tidal's design ([Tidal](https://github.com/tidalcycles/Tidal)); not rechecked against its source this session.

- **Rational time.** Tidal's `Time` is `Rational`. funk has `Num` only, and `Int` as its only integer type ([haskell-compat.md](haskell-compat.md)). This may block a pattern engine before higher-kinded classes do.

- **String literals as patterns.** `sound "bd sn"` relies on `IsString` and `OverloadedStrings`. With packed `String` and no overloaded literals, each pattern literal needs an explicit parse call. Friction program 1 will meet this first.

- **Monad.** Tidal's joins (`innerJoin`, `squeezeJoin`) are monadic. The 0.7 class work must cover `Monad`, not only `Functor` and `Applicative`.

### Constraints the table understates

- **The 128 MiB figure is a collection threshold, not a floor.** [memory.md](memory.md) records 64 MiB as tying 128 MiB on speed. For the SSP it is tunable. For plugins, N instances with a heap each multiply it; one shared runtime, as [embedding.md](embedding.md) plans, does not.

- **libtcc and macOS hosts.** A plugin runs under its host's code-signing entitlements. A hardened-runtime host without the JIT entitlement may refuse writable-then-executable memory. Option 2 may then fail on some macOS hosts, not only iOS. Unverified.

- **The runtime installs process-wide `SIGSEGV` and `SIGBUS` handlers** (`task_main`, `projects/funk/src/rt.c`). In a plugin or external, they replace the host's handlers. On a fault outside funk's ranges, `on_fault` resets to `SIG_DFL`, so the host's crash reporter never runs. The table lacks this row.

### Can the collector run on an export's thread?

No. Checked by code reading and ThreadSanitizer, 2026-10-28, Linux x86-64, GCC.

- A collection starts only in `alloc_slow` (`gc_due`, `projects/funk/src/rt.c`). The R4 check (`cgen_alloc_free`, `projects/funk/src/cgen.c`) rejects every allocating expression in an export and its callees.

- Strict top-level constants are evaluated in `program` before `main`. An export reads them unboxed from `G[]` (`FK_FLOATV(G[i])`). The collector does not move or free them, and marks live in a side array per chunk, so these reads do not race with marking.

- `tests/ffi/export.fk` under TSan: no reports, with or without `FUNK_GCSTRESS=1`.

One race remains. A C import called inside an export is wrapped in `fk_ffi_enter` and `fk_cb_check`. These read `fk_snap_lo` and `fk_cb_failed`, plain globals written on funk's thread. A copy of `export.fk` whose `sample` calls `sinf` reports a data race on `fk_snap_lo` under `FUNK_GCSTRESS=1`: `scan_snapshot` writes it, `fk_ffi_enter` on the audio thread reads it.

- `fk_cb_failed` has the same pattern, but is written only when a callback fails. TSan did not observe that write, so it is untested, not shown race-free.

- Effect today: none observed. Both TSan runs printed the expected output: the export's counter ended at 1,280,000 and the shared counter at 2,560,000. If the audio thread's frame lies above `fk_snap_lo`, it calls `fk_snapshot_release`, which returns after `fk_on_funk_thread`. Under C11 it is undefined behavior.

- Fixed (2026-10-28): `fk_snap_lo` is read and written with relaxed atomics. On x86-64 the export path compiles to the same instructions. The store that ends a snapshot is out of line (`snap_done`): inlined into `drain`, it made GCC reload `gcstress` once per marked object.

- Test: `export.fk`'s export now calls `abs`. `ffi/export/tsan-compiled` runs it under ThreadSanitizer and `FUNK_GCSTRESS=1`, where the compiler has `-fsanitize=thread`; under `setarch -R` on Linux, since TSan rejects some kernels' ASLR layouts.

Fixed (2026-10-28): the `FUNK_GCSTRESS` verifier (`verify_marks`) scanned the stack from its own frame, so it read the collector's frames in `alloc_slow` too. In a TSan build, `gc_step` stored its `gcstats` flag as one byte into an 8-byte slot whose other 7 bytes held an old pointer; the slot read as a pointer into an unreachable object, and the verifier aborted. The collector was unaffected: such a word only retains garbage. The verifier now unwinds to `alloc_slow`'s caller and starts there, with the callee-saved registers `alloc_slow` saved. It changes only stress-mode code; `alloc_slow` is found by its assembler name, since taking its address in C added an `endbr64` to it. With the failing layout forced, aborts fell from 11/30 runs to 0/30. A removed write barrier and a stack snapshot missing its newest frames are caught as before.

### Alternative framing

The open question "pattern language, DSP language, or both" has a fourth answer. funk is the compiled DSP and voice layer, driven by an existing pattern engine (Tidal, Strudel, `mhs-midi`) through the C ABI or OSC. It uses what funk already does well: ahead-of-time compilation and allocation-free exports. It defers the interpreter decision. It weakens the claim that Tidal users are the target.

### Next step

Give each friction-log program a pass/fail threshold, e.g. reload under 100 ms, or two instances with no shared mutable state. Without one, the log cannot decide between language features and domain work.

## Addendum: own pattern engine, 2026-10-28

funk gets its own pattern engine; Tidal is a reference, not a specification. This supersedes "Tidal requirements the Correction omits" and "Alternative framing" above. Design questions and the friction program's thresholds are in [patterns.md](patterns.md).
