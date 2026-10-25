# Language design

Decisions behind funk 0.1, with the alternative each one was chosen over. Implementation is in [architecture.md](architecture.md); user-facing behaviour is in the [README](../../README.md).

## Goals

- Haskell surface syntax: equations, layout, `::` for annotations, `:` for cons.

- Currying, Hindley-Milner inference, optional signatures.

- `.` for composition and `|>` for pipelines.

- A REPL and native executables from one implementation.

- Small: about 6,900 lines of C and funk.

## Evaluation: strict

Call-by-value, arguments left to right, chosen over call-by-need.

- Laziness needs a thunk and an update for every non-trivial argument, in both backends. That roughly doubles interpreter and runtime size.

- Strictness gives predictable memory use and direct C interop.

Consequences:

- Recursive bindings must be functions. `xs = 1 : xs` would read an uninitialized value, so the checker rejects it. OCaml has the same rule.

- `&&` and `||` short-circuit only when written infix: the parser turns them into `if`. `(&&)` as a value is a strict prelude function.

- Exception: non-recursive `let` and `where` value bindings are memoized thunks, forced on first use. Without this, a `where` binding runs before the guards that protect it. Only bindings already in value form (lambda, literal, constructor) skip the thunk. The cost is one allocation per evaluated binding. A `where` binding in a 5M-iteration loop costs 24% interpreted and 6x compiled (0.01 s to 0.06 s). Effects bound by `let` now run at first use rather than in binding order.

- Infinite lists, `iterate` and `cycle` do not exist.

## Effects: impure primitives sequenced by `>>`

`print`, `putStrLn` and `getLine ()` act when evaluated. `a >> b` evaluates `a`, then `b`. This was chosen over an `IO` type because a monadic `IO` without type classes needs its own bind operator and `do` desugaring, and strictness already fixes evaluation order.

Haskell's `>>` and `print` spellings are kept, so typical `main` code reads the same. Haskell's purity guarantee is lost: a function's type does not show whether it prints.

Top-level values run in dependency order, with `main` moved last unless another binding uses it.

A `do` block is syntax for `case`: each statement `p <- e`, or `e` alone, becomes `case e of p -> rest`. `case` evaluates its scrutinee first, so statements run in order, and the rest of the block stays in tail position. A strict primitive `bindNow e (\x -> rest)` was rejected, because the rest would run inside the primitive's C call, one level deeper per statement. Desugaring statements to `>>` had the same problem, since `>>` evaluates the rest as an argument. Without an `IO` type, `<-` accepts any expression: `x <- 1 + 2` is valid. A `let` statement stays lazy, as everywhere else.

## Overloading: type classes with dictionary passing

`Eq`, `Ord`, `Show` and `Num` are classes, and users can declare more. Each constraint becomes a dictionary argument. This replaces two 0.1 mechanisms: a `Num` flag on type variables, and structural `==`/`show` over runtime tags. The design and the rejected alternatives are in [type-classes.md](type-classes.md).

Consequences:

- `deriving (Eq, Ord, Show)` is required to compare or print a data type, as in Haskell. Implicit deriving was rejected: it would make every type comparable, including types that hold functions.

- A literal has type `Num a => a` and becomes `fromInt d n` in overloaded code, so it has the right representation at every type.

- Overloaded code would be slower than 0.1's tag dispatch. Specialization removes most of that cost: a call whose dictionaries are all known calls a clone of the function at those types. Inside it, prelude operators at `Int`, `Float` and `Char` become primitive calls.

### Monomorphism restriction

A binding that is not syntactically a lambda does not generalize its constrained variables. Later uses fix them, else a `Num` variable defaults to `Int` at the end of the module. Without this, `n = 1` would take a dictionary and be recomputed at every use. GHC applies the same restriction for the same reason.

Local functions without a signature do not generalize constrained variables either, which goes further than Haskell. They take those types from their uses. When the enclosing function is specialized, they are specialized with it. Generalizing them would leave `where go` loops passing dictionaries at every iteration: `trees` ran 2x slower that way. The cost is that a local function used at two number types needs a signature.

## Equality and ordering at runtime

Instances for `Int`, `Float` and `Char` call primitives that compare tagged words. Derived instances compare field by field, through the fields' instances, and order constructors by declaration order: `False < True` and `[] < (x:xs)`, as in Haskell. The REPL falls back to the structural `show` in `rt.c` for a result type that has no `Show` instance.

## Syntax choices

- `::` annotates and `:` conses, as in Haskell.

- Forward composition is `>>>` (infixr 1, as in `Control.Arrow`), since `>>` is sequencing.

- `p.x` is field access and `f . g` is composition; whitespace decides, as in GHC's `OverloadedRecordDot`. Fields belong to their type, chosen over Haskell 98's global selectors so that types can share field names. See [records.md](records.md).

- `|>` is infixl 1, like Haskell's `&`, so `xs |> map f |> sum` associates to the left.

- Fixities of built-in operators are fixed in a table. `infixl`, `infixr` and `infix` declarations are top-level only, and an operator keeps its fixity for the rest of the session. The parser installs them by pre-scanning the module's tokens before parsing. The alternative was to parse operator chains flat and re-associate them after the module is read. That supports local fixity declarations, but it needs a second pass over every expression. Haskell programs rarely declare fixities locally.

- The layout rule follows Haskell 2010 section 10.3. The standard's `parse-error(t)` clause is approximated by fixed tokens: `in`, closing brackets, `,`, `then` and `else` close implicit blocks. This covers the forms in `tests/`; other forms that depend on `parse-error(t)` need explicit braces.

## Type signatures

Signature variables are rigid (skolem constants). `f :: a -> b; f x = x` is rejected. A signature variable that unifies with a variable from an enclosing scope is an escape error. funk has no `ScopedTypeVariables`: each signature quantifies its own variables.

A binding with a signature uses that signature for recursive calls, which permits polymorphic recursion.

## Two backends, one semantics

An interpreter gives the REPL, and a compiler to C gives fast executables. Two backends can disagree, so they share as much as possible:

- One front end through closure conversion (`resolve.c`).

- One runtime (`rt.c`): values, application, primitives, `show`, comparison, GC. Compiled programs embed the same source.

- Every run test executes under both backends against one expected output.

Compiling REPL input with `cc` and loading it with `dlopen` was rejected: it adds 50-200 ms per input line and complicates keeping REPL state.

## Runtime representation: uniform tagged words

All values are one machine word: tagged 63-bit Int, tagged Char, or a pointer to a header-prefixed object. This was chosen over type-specialized unboxed code because one representation lets both backends share the runtime and its primitives, and lets the GC scan without type information. The costs are a boxed `Float` for every float result and 63-bit `Int`.

## Memory: conservative mark-sweep

The collector scans the funk thread's C stack and registers conservatively. This was chosen over a precise collector because a precise collector needs a shadow stack or stack maps in generated C and in every runtime function that holds a value. The costs:

- Objects cannot move, so the heap is not compacted.

- A word that looks like a pointer keeps an object alive.

## Deep recursion: a large stack

Strict `map` and `foldr` recurse once per list element. funk runs on a thread with a 4 GiB stack that is reserved but committed only as used, and reports overflow as a runtime error. This was chosen over CPS conversion or tail recursion modulo cons, which would complicate both backends. Measured on `count n = 1 + count (n - 1)`, the interpreter uses about 400 bytes per level, or about 2 million levels per GiB. Compiled code uses less.
