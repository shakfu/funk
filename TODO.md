# TODO

Next steps, highest value first. Phases beyond these are in
[docs/dev/roadmap.md](docs/dev/roadmap.md).

1. [ ] **Type classes.** Dictionary passing, phase 1; see [docs/dev/type-classes.md](docs/dev/type-classes.md). Resolve its open questions first.

Smaller items:

- [ ] Type errors point at the binding (`f x = x` reports line 2:1), not the sub-expression. Track the location of the expression being unified.

- [ ] Generated C triggers `-Wall` warnings: unused labels and temporaries in `cg_case`.

- [ ] Generated C includes every prelude function; emit only those reachable from `main`.

- [ ] The interpreter spends 26% of its time in `eval`'s prologue and epilogue; split out the hot cases.
