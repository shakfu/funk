# TODO

Always organize according to sections descending priority (Critical, High, Medium, Low). The can include which can include subsections as required for organization.

The primary goal is generated C speed: a geometric mean of 2x over C on the benchmark suite, 2.6x now. See [docs/dev/performance-first.md](docs/dev/performance-first.md).

## Critical

- [ ] **Typed backend.** C scalars (`int64_t`, `double`, `bool`) in monomorphic code, `V` at polymorphic boundaries, workers with typed parameters, unboxed scalar fields. `loop` is at C speed after step 2. Design: [docs/dev/typed-backend.md](docs/dev/typed-backend.md). Steps 1-2 (types on every expression, typed locals) are done; next, workers with typed parameters.

## High

- [ ] **Allocation: nursery or Perceus.** After the marking fixes, allocation and sweeping are 15-25% of `lists`, `sort` and `strings`. A bump-allocated nursery with mostly-copying collection, or reference counting with in-place reuse. Needs a design note.

- [ ] **Unknown calls.** `fk_apply` is 10-15% of `lists` and `strings`: calls to function arguments such as `map f`. Specialize higher-order functions on known function arguments, or defunctionalize.

- [ ] **Strings.** `[Char]` costs 3 words per character (`strings` 5.7x). Decide on a packed string type; see the decisions in performance-first.md.

- [ ] Generated C triggers `-Wall` warnings: unused labels and temporaries in `cg_case`.

## Medium

- [ ] **Type classes, phase 2.** Kinds and higher-kinded classes (`Functor`, `Foldable`); see [docs/dev/type-classes.md](docs/dev/type-classes.md#later-phases).

## Low

- [ ] The interpreter spends 26% of its time in `eval`'s prologue and epilogue; split out the hot cases. Interpreter speed is not part of the primary goal.
