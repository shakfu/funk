# Language design

Decisions behind funk 0.1, with the alternative each one was chosen over.
Implementation is in [architecture.md](architecture.md); user-facing
behaviour is in the [README](../../README.md).

## Goals

- Haskell surface syntax: equations, layout, `::` for annotations, `:` for
  cons.
- Currying, Hindley-Milner inference, optional signatures.
- `.` for composition and `|>` for pipelines.
- A REPL and native executables from one implementation.
- Small: about 4,700 lines of C and funk.

## Evaluation: strict

Call-by-value, arguments left to right, chosen over call-by-need.

- Laziness needs a thunk and an update for every non-trivial argument, in
  both backends. That roughly doubles interpreter and runtime size.
- Strictness gives predictable memory use and direct C interop.

Consequences:

- Recursive bindings must be functions. `xs = 1 : xs` would read an
  uninitialized value, so the checker rejects it. OCaml has the same rule.
- `&&` and `||` short-circuit only when written infix: the parser turns
  them into `if`. `(&&)` as a value is a strict prelude function.
- `where` bindings are evaluated before guards. Haskell code that relies on
  an unused `where` binding never being computed can fail. See
  [TODO.md](../../TODO.md) item 1.
- Infinite lists, `iterate` and `cycle` do not exist.

## Effects: impure primitives sequenced by `>>`

`print`, `putStrLn` and `getLine ()` act when evaluated. `a >> b` evaluates
`a`, then `b`. This was chosen over an `IO` type because a monadic `IO`
without type classes needs its own bind operator and `do` desugaring, and
strictness already fixes evaluation order.

Haskell's `>>` and `print` spellings are kept, so typical `main` code reads
the same. Haskell's purity guarantee is lost: a function's type does not
show whether it prints.

Top-level values run in dependency order, with `main` moved last unless
another binding uses it.

## Numbers: one `Num` constraint instead of type classes

`+ - *` have type `Num a => a -> a -> a`. `Num a` limits `a` to `Int` or
`Float`, and a constrained variable unifies only with those, or with
another variable, which inherits the constraint. Integer literals get a
fresh `Num` variable.

Two alternatives were rejected:

| Option | `Int + Float` | Cost |
|-|-|-|
| Join constraint, `a -> b -> join a b` | accepted | Deferred constraints, defaulting rules, about 2x the inference code; unresolved joins inside polymorphic code need runtime tags or monomorphization |
| Type classes (`Num`, `Eq`, `Show`) | type error | Dictionary passing and defaulting; several times the checker size |
| `Num` constraint (chosen) | type error, write `toFloat n + x` | One flag per type variable |

Polymorphic literals cover most mixed cases: `x * 2` and `2.5 + 1` check.

At runtime every number carries a tag. Primitives dispatch on it: Int with
Int gives Int, anything with a Float gives Float. Polymorphic code needs no
dictionaries this way.

The cost is that a literal inside a function that stays polymorphic is
built as an `Int`, even when a caller uses the result as a `Float`.
Arithmetic stays correct because Float primitives accept Int tags. Printing
does not: such a value prints without `.0`.

### Monomorphism restriction

A binding that is not syntactically a lambda does not generalize its `Num`
variables. Later uses fix them, else they default to `Int` at the end of
the module. Without this, `n = 1` would be polymorphic and stored as an
`Int` even when every use is `Float`. GHC applies the same restriction for
a related reason (sharing).

## Equality, ordering and `show`: structural

`==`, `<`, `compare`, `show` and `print` have types like `a -> a -> Bool`
and walk the runtime representation. Comparing functions is a runtime
error. This replaces derived `Eq`/`Ord`/`Show` instances. `deriving` is
parsed and ignored so Haskell sources still load.

Constructor order defines ordering: `False < True` and `[] < (x:xs)`,
matching Haskell's derived `Ord`.

`show` cannot see static types. An empty `[Char]` therefore prints `[]`.

## Syntax choices

- `::` annotates and `:` conses, as in Haskell.
- Forward composition is `>>>` (infixr 1, as in `Control.Arrow`), since
  `>>` is sequencing.
- `|>` is infixl 1, like Haskell's `&`, so `xs |> map f |> sum` associates
  to the left.
- Fixities of built-in operators are fixed in a table. User operators are
  `infixl 9`, and `infixl`/`infixr` declarations are not yet supported.
- The layout rule follows Haskell 2010 section 10.3. The standard's
  `parse-error(t)` clause is approximated by fixed tokens: `in`, closing
  brackets, `,`, `then` and `else` close implicit blocks. This covers the
  forms in `tests/`; other forms that depend on `parse-error(t)` need
  explicit braces.

## Type signatures

Signature variables are rigid (skolem constants). `f :: a -> b; f x = x`
is rejected. A signature variable that unifies with a variable from an
enclosing scope is an escape error. funk has no `ScopedTypeVariables`: each
signature quantifies its own variables.

A binding with a signature uses that signature for recursive calls, which
permits polymorphic recursion.

## Two backends, one semantics

An interpreter gives the REPL, and a compiler to C gives fast executables.
Two backends can disagree, so they share as much as possible:

- One front end through closure conversion (`resolve.c`).
- One runtime (`rt.c`): values, application, primitives, `show`,
  comparison, GC. Compiled programs embed the same source.
- Every run test executes under both backends against one expected output.

Compiling REPL input with `cc` and loading it with `dlopen` was rejected:
it adds 50-200 ms per input line and complicates keeping REPL state.

## Runtime representation: uniform tagged words

All values are one machine word: tagged 63-bit Int, tagged Char, or a
pointer to a header-prefixed object. This was chosen over
type-specialized unboxed code because structural `show`/`==` and `Num`
dispatch need tags, and one representation lets both backends share the
runtime. The costs are a boxed `Float` for every float result and 63-bit
`Int`.

## Memory: conservative mark-sweep

The collector scans the funk thread's C stack and registers conservatively.
This was chosen over a precise collector because a precise collector needs
a shadow stack or stack maps in generated C and in every runtime function
that holds a value. The costs:

- Objects cannot move, so the heap is not compacted.
- A word that looks like a pointer keeps an object alive.

## Deep recursion: a large stack

Strict `map` and `foldr` recurse once per list element. funk runs on a
thread with a 4 GiB stack that is reserved but committed only as used, and
reports overflow as a runtime error. This was chosen over CPS conversion or
tail recursion modulo cons, which would complicate both backends. Measured
on `count n = 1 + count (n - 1)`, the interpreter uses about 400 bytes per
level, or about 2 million levels per GiB. Compiled code uses less.
