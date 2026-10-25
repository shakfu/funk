# Changelog

Format: [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [Unreleased]

### Changed

- `Int` is 64 bits in both backends and wraps on overflow. Before, the interpreter wrapped at 63 bits while compiled code computed in 64 bits and truncated to 63 when an `Int` was stored in a list or passed to polymorphic code, so results after an overflow differed between backends and within compiled code. Values beyond 63 bits are boxed; integer literals and `readInt` accept the full 64-bit range. Chosen over 63-bit wrapping in compiled code, which measured 29% slower against 5.4% for boxing. The geometric mean over C went from 1.37x to 1.48x.

- Constructor fields declared `Float` are stored unboxed in compiled code: `data Vec = Vec Float Float` takes 3 words instead of 3 words plus two 2-word boxes, and reading a field does not follow a pointer. A program building 2M such values runs 2.8x faster. The interpreter keeps boxed fields.

- Fixed: a negative-zero literal, `(-0.0)`, compiled to `0.0`. The code generator printed Float literals with `%g`, which renders `-0.0` as the integer `-0`; it now emits hex float literals, which are exact.

- Closures and thunks store captured `Int`, `Float`, `Char` and `Bool` values raw instead of boxed, so capturing a `Float` no longer allocates. The benchmarks do not change; a program building 2M closures over `Float`s runs 8% faster.

- Compiled functions take and return C scalars: each is a worker with typed parameters, called directly, and a uniform wrapper for closures and unknown calls. clang can now inline small functions such as `even` and `compare`. The geometric mean over C went from 1.52x to 1.37x; `fib` is at C speed. One effect: clang turns some non-tail recursions, such as `1 + f (n - 1)`, into loops, so compiled code may run them in constant stack where the interpreter overflows.

- Compiled and interpreted code fuse chains of prelude list functions into one loop, without the intermediate lists. Chains are built from `enumFromTo`, `replicate` or any list, through `map`, `filter`, `take`, `takeWhile` and `concatMap`, into `foldl`, `sum`, `length`, `elem`, `any` or `all`, or back into a list. A chain is fused only when the result, the error raised and termination are unchanged: no stage performs an effect, at most one stage can fail, and no stage before `take` or another short circuit can fail. Exact semantics were chosen over Haskell-style imprecise errors, which would fuse more chains but make error messages depend on the optimizer. `FK_NOFUSE=1` turns fusion off. The benchmark `lists` is split: `pipeline` is the old program, compared with a fused C loop, and `lists` names its lists so they stay. The geometric mean over C went from 1.68x to 1.52x; `queens` from 2.9x to 1.4x. Design: [docs/dev/fusion.md](docs/dev/fusion.md).

- Compiled code writes the output of `map`, `filter`, `take`, `zip` and similar functions into the cells of their input list, when that list is fresh: the direct result of a call that builds new cells, such as `filter even (enumFromTo 1 n)`. A static check proves that nothing else references the input; a variable never counts as fresh. Chosen over Perceus reference counting, which a prototype showed slower: freeing cells as they die scatters later allocations. Separately, an alternative that rebuilds the cell it matched (`merge`'s `x : xs`) returns that cell. The geometric mean over C went from 1.79x to 1.68x; `sort` is 30% faster. Design: [docs/dev/reuse.md](docs/dev/reuse.md).

- `String` is a packed builtin type, no longer `[Char]`. `++` and `length` are class methods with instances for lists and `String`; `words`, `lines`, `unwords`, `unlines` and the new `concatStrings` are primitives. List functions need `unpack :: String -> [Char]`, and `pack` converts back. A string literal pattern compares by equality. Chosen over keeping `[Char]` for speed: one 3-word cell per character becomes one byte. `strings` runs at 1.6x a packed C version; it was 3.3x a `[Char]` C version.

- Generated code is faster: on the benchmark suite, its geometric mean over C went from 10.3x to 1.48x (`docs/dev/performance-first.md`). Local bindings that cannot fail, loop or act are evaluated where they are bound, without a thunk. Calls with closed function arguments, such as `map (* 3) xs`, call copies of the callee in which the argument is a known function. Functions that build a constructor around a call to themselves, such as `map`, `filter` and `++`, compile to loops (tail recursion modulo cons), so a long `map` no longer uses stack proportional to its length in compiled code. Local functions that are only called become top-level functions taking their free variables, and lambdas that capture locals are specialized like closed ones. Values of type `Int`, `Float`, `Char` and `Bool` are C scalars inside function bodies (`docs/dev/typed-backend.md`). Typed primitives are inlined, comparisons in conditions compile to C comparisons, stack overflow is detected by a guard region instead of a check in every function, and only reachable code is emitted. The collector finds objects by address arithmetic instead of binary search, skips non-pointers, no longer zeroes dead objects, and collects after max(128 MiB, 2x live) instead of max(32 MiB, live), which roughly doubles peak memory.

- `Float` comparison follows IEEE in both backends: `NaN == NaN` is `False`. It was `True`, because floats were compared structurally.

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

- Type errors are reported at the sub-expression with the wrong type, not at the binding. `f :: Int -> Bool; f x = x + 1` reported `expected Int -> Bool, found Int -> Int` at the start of `f`, and now reports `expected Bool, found Int` at `x + 1`. Expected types are now pushed into lambdas, `let`, `if`, `case` and constructor arguments, so a signature's types are known before the body is inferred. Before, `m :: Int -> String; m n = case n of 0 -> "z"; _ -> n` reported `no instance Num [Char]`, because `n` had already been unified with `String`. Rigid type variables print with their names from the signature.

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
