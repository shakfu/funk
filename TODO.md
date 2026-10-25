# TODO

Always organize according to sections descending priority (Critical, High, Medium, Low). The can include which can include subsections as required for organization.

The primary goal is generated C speed: a geometric mean of 2x over C on the benchmark suite. It is 1.89x now; the remaining items lower it further. See [docs/dev/performance-first.md](docs/dev/performance-first.md).

## Critical

- [ ] **Typed backend.** C scalars (`int64_t`, `double`, `bool`) in monomorphic code, `V` at polymorphic boundaries, workers with typed parameters, unboxed scalar fields. `loop` is at C speed after step 2. Design: [docs/dev/typed-backend.md](docs/dev/typed-backend.md). Steps 1-2 (types on every expression, typed locals) are done; next, workers with typed parameters.

## High

- [ ] **Perceus reference counting.** Collection is about 35% of `lists`, `sort` and `strings`, mostly marking live data; a nursery would not help ([docs/dev/memory.md](docs/dev/memory.md#after-trmc)). Perceus removes marking and sweeping, but is the largest change on this list: design it before writing code.

- [ ] **Strings: decision needed.** `strings` is 3.3x, but its C version also uses `[Char]` lists, so packing is not what the gap is. Options and the language consequences are in [docs/dev/strings.md](docs/dev/strings.md).

- [ ] Generated C triggers `-Wall` warnings: unused labels and temporaries in `cg_case`.

## Medium

- [ ] **Type classes, phase 2.** Kinds and higher-kinded classes (`Functor`, `Foldable`); see [docs/dev/type-classes.md](docs/dev/type-classes.md#later-phases).

## Low

- [ ] The interpreter spends 26% of its time in `eval`'s prologue and epilogue; split out the hot cases. Interpreter speed is not part of the primary goal.
