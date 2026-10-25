# TODO

Always organize according to sections descending priority (Critical, High, Medium, Low). The can include which can include subsections as required for organization.

The primary goal is generated C speed: a geometric mean of 2x over C on the benchmark suite. It is 1.35x now; the remaining items lower it further. See [docs/dev/performance-first.md](docs/dev/performance-first.md).

## Critical

- [ ] **Typed backend.** C scalars (`int64_t`, `double`, `bool`) in monomorphic code, `V` at polymorphic boundaries, workers with typed parameters, unboxed scalar fields. Design: [docs/dev/typed-backend.md](docs/dev/typed-backend.md). Steps 1-6 are done.
- [ ] 64-bit `Int` costs 5.4% at `V` boundaries (`lists` +15%, `pipeline` +28%): the range check in `FK_MKINT` and the branch in `FK_INT`. Typed list heads or `__builtin_expect` may recover part; [docs/dev/typed-backend.md](docs/dev/typed-backend.md#integers).
- [x] Copies made by `ho_specialize` get the call site's ground types: `flip@{0=primMulInt}` takes `int64_t`. Geometric mean -5.7% (`pipeline` -47%, `closures` -4%, `sort` +2.5%).
- [ ] Nullary constructors as constants in generated C, so clang can resolve a `case` on an inlined `compare` (5% on `sort` in a hand prototype).

## High

- [x] **In-place reuse of unique list cells.** Done: 1.79x to 1.68x ([docs/dev/reuse.md](docs/dev/reuse.md#result)).

- [x] **List fusion.** Done: 1.68x to 1.52x; `queens` 2.9x to 1.4x ([docs/dev/fusion.md](docs/dev/fusion.md#result)).
- [ ] Fusion: `enumFromThenTo` as a producer, and nested `concatMap`.

- [ ] Generated C triggers `-Wall` warnings: unused labels and temporaries in `cg_case`.

## Medium

- [ ] **Foreign function interface**, for libm, sqlite3 and libremidi in that order. Design and open questions: [docs/dev/ffi.md](docs/dev/ffi.md). Depends on the typed-effects decision.

- [ ] **Type classes, phase 2.** Kinds and higher-kinded classes (`Functor`, `Foldable`); see [docs/dev/type-classes.md](docs/dev/type-classes.md#later-phases).

## Low

- [ ] `showPrec` for `Float` omits the parentheses around `-0.0` (`Vec -0.0 1.0`); Haskell prints `Vec (-0.0) 1.0`. Needs an `isNegativeZero` check in the prelude instance.
- [ ] `printing` is 1.9x C. Its `enumFromTo` list is built and kept live, because `mapM_`'s function prints, so fusion refuses it: the collector is about 25% at 20M lines. `show` at `Int` goes through `vfprintf` and `malloc`, and `++` copies, about 20% each. Typed effects would let fusion treat the stage function as pure ([docs/dev/typed-effects.md](docs/dev/typed-effects.md)).
- [ ] The interpreter spends 26% of its time in `eval`'s prologue and epilogue; split out the hot cases. Interpreter speed is not part of the primary goal.
