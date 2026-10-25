# TODO

Always organize according to sections descending priority (Critical, High, Medium, Low). The can include which can include subsections as required for organization.

## Critical

## High

- [ ] **Type classes, phase 2.** Kinds and higher-kinded classes (`Functor`, `Foldable`); see [docs/dev/type-classes.md](docs/dev/type-classes.md#later-phases).

- [ ] Generated C triggers `-Wall` warnings: unused labels and temporaries in `cg_case`.

- [ ] Generated C includes every prelude function; emit only those reachable from `main`.

## Medium

- [ ] Type errors point at the binding (`f x = x` reports line 2:1), not the sub-expression. Track the location of the expression being unified.

- [ ] The interpreter spends 26% of its time in `eval`'s prologue and epilogue; split out the hot cases.

## Low





