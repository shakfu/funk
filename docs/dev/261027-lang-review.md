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
