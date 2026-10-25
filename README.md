# funk

A minimal strict functional language with Haskell syntax, written in C11. It has a REPL interpreter and a compiler to C.

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

```text
90
[3.14159,7.0]
"42"
```

## Build and run

```sh
make                          # builds ./funk; needs a C11 compiler and pthreads
make test                     # interpreter, compiled, error and REPL tests
make bench                    # time funk against C; see bench/README.md

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

- Equations with pattern matching and guards; `where`, `let ... in`, `case ... of`, `if ... then ... else`, `\x y -> e`.

- Layout (indentation) rule, or explicit `{ ; }`.

- `data T a = A a | B deriving (...)`. `deriving` is accepted and ignored.

- Optional signatures: `f :: Num a => a -> a`, `(e :: Int)`.

- Lists `[1, 2]`, ranges `[1..n]` and `[1, 3..n]`, tuples up to 7, strings as `[Char]`, sections `(+ 1)`, `(1 +)` and `` (`div` 2) ``, operator definitions `x <+> y = ...`.

Records have named fields, which belong to their type:

```haskell
data Shape = Circle { radius :: Float } | Rect { w, h :: Float }

Shape.area (Circle r) = 3.14159 * r * r   -- a function in Shape's namespace
Shape.area (Rect w h) = w * h

main =
  let r = Rect { w = 2.0, h = 3.0 }
  in print r.w >>                     -- field access
     print r.area >>                  -- Shape.area r
     print (map (.area) [r]) >>       -- (.x) is \s -> s.x
     print (r { h = 1.0 })            -- update: Rect {w = 2.0, h = 1.0}
```

`p.x` needs the type of `p`. A function like `getW s = s.w` needs a signature, except inside `Shape.f`, where an unconstrained first parameter is a `Shape`. Details are in [docs/dev/records.md](docs/dev/records.md).

Function operators:

| Operator | Meaning | Fixity |
|-|-|-|
| `f . g` | `\x -> f (g x)` | infixr 9 |
| `x \|> f` | `f x` | infixl 1 |
| `f >>> g` | `\x -> g (f x)` | infixr 1 |
| `a >> b` | evaluate `a`, then `b` | infixl 1 |
| `f $ x` | `f x` | infixr 0 |

Other built-in operators have Haskell's fixities. User-defined operators default to `infixl 9`. A top-level declaration sets the fixity of operators bound in the same module:

```haskell
infixr 5 +++
infixl 7 `times`
```

Not supported:

- type classes, apart from the `Num` constraint

- modules and imports

- list comprehensions

- `do`

- as-patterns

- laziness

## Types

Hindley-Milner inference with let-polymorphism. Signatures are optional and checked against the inferred type.

`Num a` restricts `a` to `Int` or `Float`. It replaces type classes for arithmetic:

- `+ - *`, `negate`, `abs` have type `Num a => a -> a -> a`. Both operands have one type, so `Int` + `Float` is a type error; write `toFloat n + x`.

- Integer literals are polymorphic: `x + 1` and `2.5 * 2` both check.

- `/` is `Float` only; `div`, `mod`, `quot` and `rem` are `Int` only.

- A binding that is not a function (`n = 1`) gets its number type from its uses, or defaults to `Int`.

`==`, `<`, `compare`, `show` and `print` work on every type except functions.

## Semantics

- Strict, left-to-right evaluation. `print`, `putStrLn` and `getLine ()` act when evaluated. `a >> b` orders effects.

- Top-level values are evaluated in dependency order, with `main` last.

- Recursive bindings must be functions; `xs = 1 : xs` is rejected.

- Local value bindings in `let` and `where` are evaluated on first use, at most once. `f x | x == 0 = 0 | otherwise = y where y = div 1 x` returns 0 for `x == 0`. An effect bound this way runs when the binding is first used, not where it is defined.

- Constructor fields are strict: `let (a, b) = (head [], 1) in b` fails.

- A `case` or function that does not cover every constructor gets a compile-time warning naming an unmatched value. A guarded alternative counts as covering only if one of its guards is `otherwise` or `True`.

- Tail calls use constant stack in the interpreter. Compiled code turns self tail calls into loops.

- Deep non-tail recursion has a 4 GiB stack. Overflow is a runtime error.

## Known limitations

- `.` without spaces is field access: `f.g` is no longer composition. Write `f . g`.

- `show` and `print` use the static type at the call site. Inside a polymorphic function they cannot. There, a literal used as a `Float` prints without `.0`, and `""` prints `[]`. With `one _ = 1` and `sh x = show x`, `sh (one () :: Float)` gives `"1"`, but `show (one () :: Float)` gives `"1.0"`.

- `Int` is 63 bits and wraps on overflow. `Char` is a byte; strings are not UTF-8 aware.

- The interpreter is about 6x slower than compiled code on allocation-heavy loops.

## Documentation

| File | Contents |
|-|-|
| [docs/dev/design.md](docs/dev/design.md) | language decisions and the alternatives rejected |
| [docs/dev/architecture.md](docs/dev/architecture.md) | compiler passes, runtime, GC, backends |
| [docs/dev/roadmap.md](docs/dev/roadmap.md) | planned releases |
| [bench/README.md](bench/README.md) | benchmarks against C and the results history |
| [TODO.md](TODO.md) | next steps |
| [CHANGELOG.md](CHANGELOG.md) | changes by release |
