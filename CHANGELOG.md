# Changelog

Format: [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [Unreleased]

### Changed

- `.` with no space on either side is field access: `f.g` is no longer composition.

- Non-recursive `let` and `where` value bindings are evaluated on first use, at most once. Guards can now protect a `where` binding: `f x | x == 0 = 0 | otherwise = y where y = div 1 x`. A thunk is chosen over strictness analysis for size. It costs one allocation per evaluated binding. Effects bound by `let` run at first use.

- `Eq`, `Ord`, `Show` and `Num` are type classes, replacing the `Num` flag and structural `==`/`show`. Comparing or printing a data type requires `deriving (Eq, Ord, Show)`; without it, `A == B` is a type error. `show` follows the type, so `print (one () :: Float)` prints `1.0` and `print ""` prints `""`. A constrained type nothing fixes is an error, as in Haskell: `print []`. Local functions without a signature no longer generalize constrained type variables, so specializing a function also specializes its `where` loops; one used at two number types needs a signature.

- Literals in overloaded code are built through `fromInt`, so a polymorphic literal used as a `Float` is a `Float`. 0.1 built it as an `Int`.

- `infix`, `infixl`, `infixr`, `class`, `instance` and `do` are reserved words, and `<-` is a reserved operator.

### Added

- `do` blocks with `p <- e`, expression and `let` statements. Each statement becomes `case e of p -> rest`, chosen over `>>` and over a strict bind primitive because both evaluate the rest inside a call: a recursive `do` loop of 20M iterations overflowed the 4 GiB stack, and now runs in constant stack. Without an `IO` type, `<-` accepts any expression. A `where` at the column of a block's items now closes that block.

- Type classes, phase 1 (`docs/dev/type-classes.md`): single-parameter `class` and `instance` declarations with superclasses, default methods and instance contexts, and `deriving (Eq, Ord, Show)`. Constraints are passed as dictionaries, chosen over monomorphization because the REPL interpreter and polymorphic recursion need code that works at any type. Calls whose dictionaries are all known call a specialized clone, `fib@Int`, so monomorphic code runs as fast as in 0.1.

```haskell
class Shape a where
  area :: a -> Float
  describe :: a -> String
  describe x = "area " ++ show (area x)
```

- Records with fields scoped to their type: `data P = P { x, y :: Float }`, construction `P { x = 1.0, y = 2.0 }`, update `p { x = 3.0 }`, patterns `P { x = a }`, and dot access `p.x`, `(.x)`. `Type.f` defines a function in a type's namespace, and `p.f` calls it with `p` first. Type-scoped fields were chosen over Haskell 98's global selectors so that two types can share a field name. The cost is that `p.x` needs `p`'s type: `getX p = p.x` needs a signature. See `docs/dev/records.md`.

- Benchmark suite (`make bench`): 10 programs in funk and in C that use the same algorithm and data structures. Each run appends timings, max RSS and build times to `bench/results.jsonl`, keyed by host and commit. `bench/bench.py report` flags regressions against an earlier run.

- `infixl`, `infixr` and `infix` declarations at top level, as in Haskell 2010. Declarations are installed by pre-scanning the module, chosen over re-associating parsed operator chains because it leaves the precedence-climbing parser unchanged. The cost is no local fixity declarations.

- Compile-time warning for a `case` or function that does not cover every constructor, naming an unmatched value. Guarded alternatives count only when a guard is `otherwise` or `True`.

### Fixed

- Tuple constructors were registered with no type (`tydef` -1), because the type name was looked up without interning. The C backend read `tydefs[-1]` for every tuple pattern.

## [0.1.0]

### Added

- Language with Haskell syntax: clauses with pattern matching and guards, `where`, `let`, `case`, `if`, lambdas, layout rule, `data`, sections, ranges, operator definitions, `.`, `|>`, `>>>` and `$`.

- Hindley-Milner inference with optional, rigid signatures. Numbers use a single `Num a` constraint over `Int` and `Float` with polymorphic literals, chosen over type classes for size; see `docs/dev/design.md`.

- Strict evaluation with effects sequenced by `>>`.

- REPL (`funk`) and interpreter (`funk FILE`).

- Compiler to standalone C (`funk build`, `funk emit-c`).

- Shared runtime with a conservative mark-sweep GC and a 4 GiB lazily committed stack.

- Prelude of 78 functions over lists, `Maybe`, `Either`, characters and strings.

- Test suite (`make test`): every run test is checked under both backends.
