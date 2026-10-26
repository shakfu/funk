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

## 0.4 -- language growth

Larger changes, each needing its own design note in `docs/dev/` first.

- A foreign function interface ([ffi.md](ffi.md)): libm (phase 1), sqlite3 (phase 2) and libremidi (phase 3), done; real-time collector pauses remain ([realtime.md](realtime.md)).
- Haskell compatibility: what stops Haskell source from running, and what would close it ([haskell-compat.md](haskell-compat.md)).

- Type classes phase 2: kinds and higher-kinded classes (`Functor`, `Foldable`). Phase 3: existential types, for lists of mixed instances.

- Modules and `import`. Lowercase module names with `.` paths are reserved in [records.md](records.md#modules).

- As-patterns, list comprehensions.

## Open questions

- Should the interpreter be kept once compile-and-load is fast enough? Two backends double the cost of every semantic change.
