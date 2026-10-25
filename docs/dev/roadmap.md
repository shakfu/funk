# Roadmap

A provisional plan, ordered by value per unit of work. The concrete items for the next release are in [TODO.md](../../TODO.md). Version numbers are targets, not commitments.

## 0.1 -- done

Core language, HM inference with `Num`, interpreter, REPL, C backend, conservative GC. See [CHANGELOG.md](../../CHANGELOG.md).

## 0.2 -- remove semantic surprises

Every item here makes funk behave as Haskell programmers expect, without changing the core design.

- Lazy `where` bindings: delay non-function bindings until first use.

- Compile-time warning for non-exhaustive patterns.

- Type-directed `show`: print `Float` and `[Char]` correctly where the static type is known.

- `infixl`, `infixr` and `infix` declarations.

- Error locations at the offending sub-expression, not the binding.

## 0.3 -- performance

Measure first with `make bench`, then compare with `bench/bench.py report`. Baseline on an M1 before 0.3: compiled funk is 2.7-65x slower than equivalent C, and the interpreter is 24-203x slower. See [bench/README.md](../../bench/README.md).

- Split `eval` so the common cases avoid its large prologue (26% of interpreter time in callgrind).

- Inline Int fast paths for arithmetic and comparison in generated C.

- Drop unreachable prelude functions from generated C.

- Optional: tail recursion modulo cons for `map`-style functions, which removes the stack limit on long lists.

## 0.4 -- language growth

Larger changes, each needing its own design note in `docs/dev/` first.

- Type classes with dictionary passing. This would replace the `Num` flag and structural `show`/`==`, and fix the polymorphic-literal limitation from [design.md](design.md). It is the biggest change on this list.

- Modules and `import`.

- Records, as-patterns, list comprehensions.

- A `do` block for sequencing, desugared to `>>` and `let`.

## Open questions

- Should effects become typed (an `IO` type) once type classes exist? This would break `print x >> ...` code written against 0.1.

- Should `Int` become a full 64 bits? That would need boxing or a different tag scheme.

- Should the interpreter be kept once compile-and-load is fast enough? Two backends double the cost of every semantic change.
