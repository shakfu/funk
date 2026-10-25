# TODO

Always organize according to sections descending priority (Critical, High, Medium, Low). The can include which can include subsections as required for organization.

The primary goal is generated C speed: a geometric mean of 2x over C on the benchmark suite, 2.4x now. See [docs/dev/performance-first.md](docs/dev/performance-first.md).

## Critical

- [ ] **Typed backend.** C scalars (`int64_t`, `double`, `bool`) in monomorphic code, `V` at polymorphic boundaries, workers with typed parameters, unboxed scalar fields. `loop` is at C speed after step 2. Design: [docs/dev/typed-backend.md](docs/dev/typed-backend.md). Steps 1-2 (types on every expression, typed locals) are done; next, workers with typed parameters.

## High

- [ ] **Tail recursion modulo cons in `cgen.c`.** `map`, `filter`, `++`, `enumFromTo` and similar build lists by deep recursion, so every collection scans a deep stack. As loops with a destination pointer, `lists` ran 2.5x faster in a hand-edited experiment. Design: [docs/dev/memory.md](docs/dev/memory.md).

- [ ] **Allocation: nursery or Perceus,** decided from a profile after TRMC. Collection is about 40% of `lists` today. See [docs/dev/memory.md](docs/dev/memory.md#options).

- [ ] **Unknown calls through lambdas with free variables.** Calls with closed function arguments are specialized (`map (* 3)`). A lambda capturing a local, as in `queens`' `\q -> safe q qs`, is still called through `fk_apply`. Lambda-lift such arguments into extra parameters, then specialize.

- [ ] **Strings.** `[Char]` costs 3 words per character (`strings` 5.7x). Decide on a packed string type; see the decisions in performance-first.md.

- [ ] Generated C triggers `-Wall` warnings: unused labels and temporaries in `cg_case`.

## Medium

- [ ] **Type classes, phase 2.** Kinds and higher-kinded classes (`Functor`, `Foldable`); see [docs/dev/type-classes.md](docs/dev/type-classes.md#later-phases).

## Low

- [ ] The interpreter spends 26% of its time in `eval`'s prologue and epilogue; split out the hot cases. Interpreter speed is not part of the primary goal.
