# Changelog

Format: [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [Unreleased]

### Added

- `FUNK_GCSTATS=1` prints each collection's pause, split into roots, mark and finish, and a summary at exit, in both backends.

- Foreign imports, phase 1 of [docs/dev/ffi.md](docs/dev/ffi.md): `foreign import ccall "math.h cbrt" cbrt :: Float -> Float` calls a C function, and `foreign library "name"` links `-lname`. Arguments and results are `Int`, `Float`, `Char`, `Bool`, `()` and the C types below; `->>` marks an import that acts. An import of a non-function type reads a C value once (`"math.h M_PI"`). Compiled code calls the C function directly, as fast as a primitive. The interpreter compiles the same glue with `$CC` into a shared library, cached by its hash in `$XDG_CACHE_HOME/funk/ffi`, and loads it: about 90 ms the first time, nothing after. Compiled glue, over libffi in the interpreter, lets imports call macros and `static inline` functions, and keeps both backends' conversions identical. `funk run` and `repl` take `-I`, `-L` and `-l` for the glue, and `build` takes `-I`.
- The declared types of an import are checked against the C declaration by the C compiler: a conversion that can change a value or drop a `const` is an error, so `abs :: Int -> Int` is rejected, since `abs` takes an `int`. Widening is allowed, and a `Ptr` passes as any object pointer. An exact comparison of the function type was tried first; it cannot pass `Ptr Db` where C has `sqlite3 *`.
- Foreign imports, phase 3: callbacks. `foreign import ccall "callback" f :: FunPtr (Ptr () -> a ->> r)` is the address of a C function that runs a funk closure, passed in its context argument as a `StablePtr`, on funk's thread; `"queue"` makes one that any thread may call, whose calls `waitEvent` and `pollEvent` run later. `CStringLen` in a callback's type is a C pointer and length, received as a copied `String`. An error in a callback is raised when the foreign call returns, never across C frames. A value import may be any C expression, such as `sizeof(T)` or `offsetof(T, f)`, for `peekByteOff` and `pokeByteOff`. Also: `allocaBytes`, `newCString`, `CBool`, `newStablePtr`, `deRefStablePtr`, `freeStablePtr` and the `FunPtr` and `StablePtr` casts. libremidi works ([examples/ffi/libremidi.fk](examples/ffi/libremidi.fk)). A trampoline per callback type, with the closure in the context, needs no code generated at run time, unlike Haskell's `"wrapper"`; C APIs without a context pointer cannot call funk.
- Foreign imports, phase 2: `String` arguments and results (`const char *`), `Ptr a`, opaque types (`data Db` without constructors), and Haskell's `Storable` (`peek`, `poke`), `alloca`, `mallocBytes`, `free`, `nullPtr`, `castPtr`, `plusPtr` and `peekCString`, with `CChar`. A NUL inside a `String` passed to C, or a NULL `String` result, is a run-time error rather than a shortened string. sqlite3 works in both backends.
- `CInt`, `CUInt`, `CLong`, `CULong`, `CLLong`, `CSize` and `CFloat`, C's scalar types, with `Eq`, `Ord`, `Show` and `Num` instances whose arithmetic wraps to the C width. `CULong` and `CSize` compare and show unsigned. A new class `Integral`, with `toInt`, and `fromIntegral :: (Integral a, Num b) => a -> b` convert between integer types; `toCFloat` and `fromCFloat` between `Float` and `CFloat`. These names are now reserved. Real types, over names that mean a range-checked `Int` in foreign signatures only, keep Haskell bindings close to their source and give C's wrapping arithmetic.

### Changed

- The collector sweeps lazily: allocation sweeps a chunk when its free list runs out, so a pause is roots and marking only. The longest pause of a program with 100,000 live cells fell from 17.9 ms to 1.0 ms, and with 1,000,000 from 35.0 ms to 9.8 ms; a sweep step costs at most 0.15 ms. Allocation-heavy benchmarks ran 2-8% faster. A chunk still unswept at the next collection has its marks cleared rather than being swept, which would put the sweep back in the pause. See [docs/dev/realtime.md](docs/dev/realtime.md).

## [0.3.0] - 2026-10-26

### Changed

- Prelude functions and class methods cannot be redefined at top level: a definition of `lookup` or a method named `length` is an error, where it used to replace the prelude's. A name in a program then always means the prelude's, since without modules a shadowed prelude function could not be reached. Use another name or a type namespace (`Shape.lookup`); locals may still shadow prelude names. The prelude's types and constructors (`Maybe`, `Just`, `Ordering`) are reserved likewise.

- Effects are in types. A function arrow is `a -> b` if the function does not act by itself and `a ->> b` if it may; effects are inferred, and checked against signatures. A signature's `->` arrows share one effect, so `map :: (a -> b) -> [a] -> [b]` acts exactly when its argument does. Breaking: a signature that says `->` over a body that prints is an error; a top-level value that acts is an error, except `main` (make it a function); a `where` or `let ... in` binding that can act is an error, since its effect would run at a later use or under the wrong guard: bind it with `<-` or a `let` statement in `do`, where it runs on its line, or make it a function; a data field or class method written with `->` holds only pure functions. Chosen over Haskell's `IO` type because calls stay direct, with no runtime cost, and unannotated code needs no change. Design, prototype and alternatives: [docs/dev/typed-effects.md](docs/dev/typed-effects.md).

## [0.2.0] - 2026-10-25

### Changed

- Fresh heap chunks are allocated with a bump pointer instead of a free list built when the chunk is created, which wrote every object once before its use. The geometric mean over C improved 3%; `lists` 9%.

- Copies of higher-order functions made for a known function argument also take the call site's types, so `flip@{0=primMulInt}` works on C integers instead of boxed values. Copies are keyed by those types too, so `map id` at `[Int]` and at `[Char]` get separate copies. The geometric mean over C improved 5.7%.

- `Int` is 64 bits in both backends and wraps on overflow. Before, the interpreter wrapped at 63 bits while compiled code computed in 64 bits and truncated to 63 when an `Int` was stored in a list or passed to polymorphic code, so results after an overflow differed between backends and within compiled code. Values beyond 63 bits are boxed; integer literals and `readInt` accept the full 64-bit range. Chosen over 63-bit wrapping in compiled code, which measured 29% slower against 5.4% for boxing. The geometric mean over C went from 1.37x to 1.48x.

- Constructor fields declared `Float` are stored unboxed in compiled code: `data Vec = Vec Float Float` takes 3 words instead of 3 words plus two 2-word boxes, and reading a field does not follow a pointer. A program building 2M such values runs 2.8x faster. The interpreter keeps boxed fields.

- Fixed: a negative-zero literal, `(-0.0)`, compiled to `0.0`. The code generator printed Float literals with `%g`, which renders `-0.0` as the integer `-0`; it now emits hex float literals, which are exact.

- Closures and thunks store captured `Int`, `Float`, `Char` and `Bool` values raw instead of boxed, so capturing a `Float` no longer allocates. The benchmarks do not change; a program building 2M closures over `Float`s runs 8% faster.

- Compiled functions take and return C scalars: each is a worker with typed parameters, called directly, and a uniform wrapper for closures and unknown calls. clang can now inline small functions such as `even` and `compare`. The geometric mean over C went from 1.52x to 1.37x; `fib` is at C speed. One effect: clang turns some non-tail recursions, such as `1 + f (n - 1)`, into loops, so compiled code may run them in constant stack where the interpreter overflows.

- Compiled and interpreted code fuse chains of prelude list functions into one loop, without the intermediate lists. Chains are built from `enumFromTo`, `replicate` or any list, through `map`, `filter`, `take`, `takeWhile` and `concatMap`, into `foldl`, `sum`, `length`, `elem`, `any` or `all`, or back into a list. A chain is fused only when the result, the error raised and termination are unchanged: no stage performs an effect, at most one stage can fail, and no stage before `take` or another short circuit can fail. Exact semantics were chosen over Haskell-style imprecise errors, which would fuse more chains but make error messages depend on the optimizer. `FK_NOFUSE=1` turns fusion off. The benchmark `lists` is split: `pipeline` is the old program, compared with a fused C loop, and `lists` names its lists so they stay. The geometric mean over C went from 1.68x to 1.52x; `queens` from 2.9x to 1.4x. Design: [docs/dev/fusion.md](docs/dev/fusion.md).

- Compiled code writes the output of `map`, `filter`, `take`, `zip` and similar functions into the cells of their input list, when that list is fresh: the direct result of a call that builds new cells, such as `filter even (enumFromTo 1 n)`. A static check proves that nothing else references the input; a variable never counts as fresh. Chosen over Perceus reference counting, which a prototype showed slower: freeing cells as they die scatters later allocations. Separately, an alternative that rebuilds the cell it matched (`merge`'s `x : xs`) returns that cell. The geometric mean over C went from 1.79x to 1.68x; `sort` is 30% faster. Design: [docs/dev/reuse.md](docs/dev/reuse.md).

- `String` is a packed builtin type, no longer `[Char]`. `++` and `length` are class methods with instances for lists and `String`; `words`, `lines`, `unwords`, `unlines` and the new `concatStrings` are primitives. List functions need `unpack :: String -> [Char]`, and `pack` converts back. A string literal pattern compares by equality. Chosen over keeping `[Char]` for speed: one 3-word cell per character becomes one byte. `strings` runs at 1.6x a packed C version; it was 3.3x a `[Char]` C version.

- Generated code is faster: on the benchmark suite, its geometric mean over C went from 10.3x to 1.35x (`docs/dev/performance-first.md`). Local bindings that cannot fail, loop or act are evaluated where they are bound, without a thunk. Calls with closed function arguments, such as `map (* 3) xs`, call copies of the callee in which the argument is a known function. Functions that build a constructor around a call to themselves, such as `map`, `filter` and `++`, compile to loops (tail recursion modulo cons), so a long `map` no longer uses stack proportional to its length in compiled code. Local functions that are only called become top-level functions taking their free variables, and lambdas that capture locals are specialized like closed ones. Values of type `Int`, `Float`, `Char` and `Bool` are C scalars inside function bodies (`docs/dev/typed-backend.md`). Typed primitives are inlined, comparisons in conditions compile to C comparisons, stack overflow is detected by a guard region instead of a check in every function, and only reachable code is emitted. The collector finds objects by address arithmetic instead of binary search, skips non-pointers, no longer zeroes dead objects, and collects after max(128 MiB, 2x live) instead of max(32 MiB, live), which roughly doubles peak memory.

- `Float` comparison follows IEEE in both backends: `NaN == NaN` is `False`. It was `True`, because floats were compared structurally.

- `.` with no space on either side is field access: `f.g` is no longer composition.

- Non-recursive `let` and `where` value bindings are evaluated on first use, at most once. Guards can now protect a `where` binding: `f x | x == 0 = 0 | otherwise = y where y = div 1 x`. A thunk is chosen over strictness analysis for size. It costs one allocation per evaluated binding. Effects bound by `let` run at first use.

- `Eq`, `Ord`, `Show` and `Num` are type classes, replacing the `Num` flag and structural `==`/`show`. Comparing or printing a data type requires `deriving (Eq, Ord, Show)`; without it, `A == B` is a type error. `show` follows the type, so `print (one () :: Float)` prints `1.0` and `print ""` prints `""`. A constrained type nothing fixes is an error, as in Haskell: `print []`. Local functions without a signature no longer generalize constrained type variables, so specializing a function also specializes its `where` loops; one used at two number types needs a signature.

- Literals in overloaded code are built through `fromInt`, so a polymorphic literal used as a `Float` is a `Float`. 0.1 built it as an `Int`.

- `infix`, `infixl`, `infixr`, `class`, `instance` and `do` are reserved words, and `<-` is a reserved operator.

### Added

- A CMake build (`CMakeLists.txt`), with `make` as a frontend: it configures on first use, builds in `build/` and links `./funk`. `make SANITIZE=undefined test` builds and tests funk under a sanitizer in its own directory, and `make install` installs it. `ctest` runs the test suite.
- Command line: `funk check FILE` type-checks without running; `funk eval EXPR` (`-e`) evaluates an expression; `funk repl FILE` loads a file into the REPL. `funk build` takes `-O LEVEL`, `-l LIB` and `-L DIR`. `funk help [COMMAND]`, `-h` and `--version` print to standard output. Usage errors name the problem and the help to read, and exit with status 2; failing programs exit with 1.

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
