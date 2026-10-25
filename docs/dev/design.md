# Language design

Decisions behind funk 0.1, with the alternative each one was chosen over. Implementation is in [architecture.md](architecture.md); user-facing behaviour is in the [README](../../README.md).

## Goals

- Haskell surface syntax: equations, layout, `::` for annotations, `:` for cons.

- Currying, Hindley-Milner inference, optional signatures.

- `.` for composition and `|>` for pipelines.

- A REPL and native executables from one implementation.

- Small: about 4,700 lines of C and funk.

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

## Numbers: one `Num` constraint instead of type classes

`+ - *` have type `Num a => a -> a -> a`. `Num a` limits `a` to `Int` or `Float`, and a constrained variable unifies only with those, or with another variable, which inherits the constraint. Integer literals get a fresh `Num` variable.

Two alternatives were rejected:

| Option | `Int + Float` | Cost |
|-|-|-|
| Join constraint, `a -> b -> join a b` | accepted | Deferred constraints, defaulting rules, about 2x the inference code; unresolved joins inside polymorphic code need runtime tags or monomorphization |
| Type classes (`Num`, `Eq`, `Show`) | type error | Dictionary passing and defaulting; several times the checker size |
| `Num` constraint (chosen) | type error, write `toFloat n + x` | One flag per type variable |

Polymorphic literals cover most mixed cases: `x * 2` and `2.5 + 1` check.

At runtime every number carries a tag. Primitives dispatch on it: Int with Int gives Int, anything with a Float gives Float. Polymorphic code needs no dictionaries this way.

The cost is that a literal inside a function that stays polymorphic is built as an `Int`, even when a caller uses the result as a `Float`. Arithmetic stays correct because Float primitives accept Int tags. Type-directed `show` (below) prints such a value as a Float wherever the call site's type is known.

### Monomorphism restriction

A binding that is not syntactically a lambda does not generalize its `Num` variables. Later uses fix them, else they default to `Int` at the end of the module. Without this, `n = 1` would be polymorphic and stored as an `Int` even when every use is `Float`. GHC applies the same restriction for a related reason (sharing).

## Equality, ordering and `show`: structural, with static types for `show`

`==`, `<`, `compare`, `show` and `print` have types like `a -> a -> Bool` and walk the runtime representation. Comparing functions is a runtime error. This replaces derived `Eq`/`Ord`/`Show` instances. `deriving` is parsed and ignored so Haskell sources still load.

Constructor order defines ordering: `False < True` and `[] < (x:xs)`, matching Haskell's derived `Ord`.

Where the argument type of `show` or `print` is known at the call site, the checker passes a type descriptor, and `show` walks the value and the descriptor together. An `Int`-tagged value at a `Float` position then prints with `.0`, and an empty `[Char]` prints `""`. In polymorphic code the descriptor is absent and `show` stays structural. Type classes would remove this gap. A descriptor was chosen over them for size: about 100 lines.

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

All values are one machine word: tagged 63-bit Int, tagged Char, or a pointer to a header-prefixed object. This was chosen over type-specialized unboxed code because structural `show`/`==` and `Num` dispatch need tags, and one representation lets both backends share the runtime. The costs are a boxed `Float` for every float result and 63-bit `Int`.

## Memory: conservative mark-sweep

The collector scans the funk thread's C stack and registers conservatively. This was chosen over a precise collector because a precise collector needs a shadow stack or stack maps in generated C and in every runtime function that holds a value. The costs:

- Objects cannot move, so the heap is not compacted.

- A word that looks like a pointer keeps an object alive.

## Deep recursion: a large stack

Strict `map` and `foldr` recurse once per list element. funk runs on a thread with a 4 GiB stack that is reserved but committed only as used, and reports overflow as a runtime error. This was chosen over CPS conversion or tail recursion modulo cons, which would complicate both backends. Measured on `count n = 1 + count (n - 1)`, the interpreter uses about 400 bytes per level, or about 2 million levels per GiB. Compiled code uses less.
