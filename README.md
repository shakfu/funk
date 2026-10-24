# funk

A minimal strict functional language with Haskell syntax, written in C11.
It has a REPL interpreter and a compiler to C.

```haskell
data Shape = Circle Float | Rect Float Float

area :: Shape -> Float
area (Circle r) = 3.14159 * r * r
area (Rect w h) = w * h

f x = x + 1                       -- f :: Num a => a -> a

main =
  [1..10] |> filter even |> map (* 3) |> sum |> print >>
  print (map area [Circle 1.0, Rect 2.0 3.5]) >>
  print ((show . f) 41)
```

Output:

```
90
[3.14159,7.0]
"42"
```

## Build and run

```sh
make                          # builds ./funk; needs a C11 compiler and pthreads
make test                     # interpreter, compiled, error and REPL tests

./funk                        # REPL
./funk prog.fk                # interpret (same as: funk run prog.fk)
./funk build prog.fk -o prog  # native executable via $CC (default cc)
./funk emit-c prog.fk -o prog.c
```

REPL commands:

| Command | Effect |
|-|-|
| `:t EXPR` | print the type of `EXPR` |
| `:l FILE` | load definitions from `FILE` |
| `:{` ... `:}` | multi-line input |
| `:q` | quit |

`it` is the previous result. There is no line editing; use `rlwrap ./funk`.

## Language

Syntax follows Haskell 2010 where a feature exists:

- Equations with pattern matching and guards; `where`, `let ... in`,
  `case ... of`, `if ... then ... else`, `\x y -> e`.
- Layout (indentation) rule, or explicit `{ ; }`.
- `data T a = A a | B deriving (...)`. `deriving` is accepted and ignored.
- Optional signatures: `f :: Num a => a -> a`, `(e :: Int)`.
- Lists `[1, 2]`, ranges `[1..n]` and `[1, 3..n]`, tuples up to 7, strings
  as `[Char]`, sections `(+ 1)`, `(1 +)` and `` (`div` 2) ``, operator
  definitions `x <+> y = ...`.

Function operators:

| Operator | Meaning | Fixity |
|-|-|-|
| `f . g` | `\x -> f (g x)` | infixr 9 |
| `x \|> f` | `f x` | infixl 1 |
| `f >>> g` | `\x -> g (f x)` | infixr 1 |
| `a >> b` | evaluate `a`, then `b` | infixl 1 |
| `f $ x` | `f x` | infixr 0 |

Other built-in operators have Haskell's fixities. User-defined operators
are `infixl 9`. Fixity declarations are not supported.

Not supported:

- type classes, apart from the `Num` constraint
- records
- modules and imports
- list comprehensions
- `do`
- as-patterns
- laziness

## Types

Hindley-Milner inference with let-polymorphism. Signatures are optional
and checked against the inferred type.

`Num a` restricts `a` to `Int` or `Float`. It replaces type classes for
arithmetic:

- `+ - *`, `negate`, `abs` have type `Num a => a -> a -> a`. Both operands
  have one type, so `Int` + `Float` is a type error; write `toFloat n + x`.
- Integer literals are polymorphic: `x + 1` and `2.5 * 2` both check.
- `/` is `Float` only; `div`, `mod`, `quot` and `rem` are `Int` only.
- A binding that is not a function (`n = 1`) gets its number type from its
  uses, or defaults to `Int`.

`==`, `<`, `compare`, `show` and `print` work on every type except
functions.

## Semantics

- Strict, left-to-right evaluation. `print`, `putStrLn` and `getLine ()`
  act when evaluated. `a >> b` orders effects.
- Top-level values are evaluated in dependency order, with `main` last.
- Recursive bindings must be functions; `xs = 1 : xs` is rejected.
- `where` bindings are evaluated before guards. Here `y` is computed, and
  divides by zero, even when `x == 0`:
  `f x | x == 0 = 0 | otherwise = y where y = div 1 x`.
- Tail calls use constant stack in the interpreter. Compiled code turns
  self tail calls into loops.
- Deep non-tail recursion has a 4 GiB stack. Overflow is a runtime error.

## Known limitations

- A literal returned from a polymorphic function and used as a `Float` is
  stored as an `Int`. It computes correctly but prints without `.0`. For
  example, with `one _ = 1`, `print (one () :: Float)` prints `1`.
- `show ""` prints `[]`.
- `Int` is 63 bits and wraps on overflow. `Char` is a byte; strings are not
  UTF-8 aware.
- The interpreter is about 6x slower than compiled code on
  allocation-heavy loops.

## Documentation

| File | Contents |
|-|-|
| [docs/dev/design.md](docs/dev/design.md) | language decisions and the alternatives rejected |
| [docs/dev/architecture.md](docs/dev/architecture.md) | compiler passes, runtime, GC, backends |
| [docs/dev/roadmap.md](docs/dev/roadmap.md) | planned releases |
| [TODO.md](TODO.md) | next steps |
| [CHANGELOG.md](CHANGELOG.md) | changes by release |
