# TODO

Next steps, highest value first. Phases beyond these are in
[docs/dev/roadmap.md](docs/dev/roadmap.md).

1. **Lazy `where` bindings.** Non-function `where` and `let` bindings run
   before the guards, so `f x | x == 0 = 0 | otherwise = y where y = div 1 x`
   fails at `x == 0`. Delay them with a memoized thunk: one new object kind
   in `rt.c`, forced on variable load in `interp.c` and `cgen.c`.
2. **Non-exhaustive pattern warning.** Today a missing case is found only
   at runtime by `fk_match_fail`. Check constructor coverage per `case` in
   `check.c` after inference, when constructor ids are known.
3. **Type-directed `show`.** At call sites of `show`/`print` with a
   monomorphic argument type, pass the type so `Float` values print with
   `.0` and `""` prints as `""`. Polymorphic call sites keep structural
   `show`.
4. **Fixity declarations.** Parse `infixl`/`infixr`/`infix N op`. Fixities
   must be known before operator expressions are parsed. Either pre-scan
   the token stream, or parse operator chains flat and re-associate them
   after the module is read.
5. **Type classes.** Large; write a design note in `docs/dev/` first. See
   the roadmap.

Smaller items:

- Type errors point at the binding (`f x = x` reports line 2:1), not the
  sub-expression. Track the location of the expression being unified.
- Generated C triggers `-Wall` warnings: unused labels and temporaries in
  `cg_case`.
- Generated C includes every prelude function; emit only those reachable
  from `main`.
- The interpreter spends 26% of its time in `eval`'s prologue and
  epilogue; split out the hot cases.
