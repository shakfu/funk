# TODO

Always organize according to sections descending priority (Critical, High, Medium, Low). The can include which can include subsections as required for organization.

The primary goal is generated C speed: a geometric mean of 2x over C on the benchmark suite. It is 1.37x now; the remaining items lower it further. See [docs/dev/performance-first.md](docs/dev/performance-first.md).

## Critical

- [ ] **Typed backend.** C scalars (`int64_t`, `double`, `bool`) in monomorphic code, `V` at polymorphic boundaries, workers with typed parameters, unboxed scalar fields. Design: [docs/dev/typed-backend.md](docs/dev/typed-backend.md). Steps 1-3 (types on every expression, typed locals, workers) are done: 1.37x. Next, typed closure captures.
- [ ] Copies made by `ho_specialize` keep generic parameter types: `flip@{0=primMulInt}` takes `V` where `Int` is known.
- [ ] Nullary constructors as constants in generated C, so clang can resolve a `case` on an inlined `compare` (5% on `sort` in a hand prototype).

## High

- [x] **In-place reuse of unique list cells.** Done: 1.79x to 1.68x ([docs/dev/reuse.md](docs/dev/reuse.md#result)).

- [x] **List fusion.** Done: 1.68x to 1.52x; `queens` 2.9x to 1.4x ([docs/dev/fusion.md](docs/dev/fusion.md#result)).
- [ ] Fusion: `enumFromThenTo` as a producer, and nested `concatMap`.

- [ ] Generated C triggers `-Wall` warnings: unused labels and temporaries in `cg_case`.

## Medium

- [ ] **Type classes, phase 2.** Kinds and higher-kinded classes (`Functor`, `Foldable`); see [docs/dev/type-classes.md](docs/dev/type-classes.md#later-phases).

## Low

- [ ] The interpreter spends 26% of its time in `eval`'s prologue and epilogue; split out the hot cases. Interpreter speed is not part of the primary goal.
