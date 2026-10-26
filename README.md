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

Building needs CMake 3.20 or later, a C11 compiler and pthreads. `make` drives CMake, using Ninja when it is installed:

```sh
make                          # builds build/funk and links ./funk to it
make test                     # interpreter, compiled, error, REPL and command-line tests
make SANITIZE=undefined test  # the same, with funk built under UBSan in build-undefined/
make bench                    # time funk against C; see bench/README.md
make install PREFIX=~/.local  # install funk into PREFIX/bin
```

CMake works directly too: `cmake -B build && cmake --build build && ctest --test-dir build`. `BUILD_TYPE` (default `RelWithDebInfo`) applies when a build directory is first configured.

```sh
funk                          # REPL
funk repl prog.fk             # REPL, after loading prog.fk
funk prog.fk                  # interpret (same as: funk run prog.fk)
funk build prog.fk -o prog    # native executable via $CC (default cc)
funk build prog.fk -O3 -lsqlite3   # C optimization level; libraries to link
funk emit-c prog.fk -o prog.c # the generated C
funk check prog.fk            # type-check only, for editors
funk -e 'sum [1..10]'         # evaluate an expression (same as: funk eval EXPR)
funk help build               # a command's options; funk --version
```

Exit status: 0 on success, 1 when the program or its compilation fails, 2 on a usage error.

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

- `data T a = A a | B deriving (Eq, Ord, Show)`.

- Optional signatures: `f :: (Ord a, Num a) => a -> a`, `(e :: Int)`.

- `class` and `instance` declarations: single-parameter classes with superclasses and default methods.

- Lists `[1, 2]`, ranges `[1..n]` and `[1, 3..n]`, tuples up to 7, strings, sections `(+ 1)`, `(1 +)` and `` (`div` 2) ``, operator definitions `x <+> y = ...`.

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

`p.x` needs the type of `p`. A function like `getW s = s.w` needs a signature, except inside `Shape.f`, where an unconstrained first parameter is a `Shape`. Details are in [docs/dev/records.md](docs/dev/records.md); [examples/shop.fk](examples/shop.fk) is a longer example.

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

- multi-parameter and higher-kinded classes (`Functor`), class methods that are not functions

- modules and imports

- list comprehensions

- as-patterns

- laziness

## Types

Hindley-Milner inference with let-polymorphism. Signatures are optional and checked against the inferred type.

Type classes follow Haskell 2010 for classes over a single type:

```haskell
class Shape a where
  area :: a -> Float
  describe :: a -> String
  describe x = "area " ++ show (area x)      -- default method

data Sq = Sq Float deriving (Eq, Show)
instance Shape Sq where
  area (Sq s) = s * s
```

The prelude defines:

| Class | Methods | Instances |
|-|-|-|
| `Eq` | `==`, `/=` | `Int`, `Float`, `Char`, `Bool`, `()`, lists, tuples, `Maybe`, `Either`, `Ordering` |
| `Ord` (superclass `Eq`) | `compare`; `<`, `<=`, `>`, `>=`, `min`, `max` are functions over it | as `Eq` |
| `Show` | `showPrec`, `show`, `showList` | as `Eq` |
| `Num` | `+`, `-`, `*`, `negate`, `abs`, `fromInt` | `Int`, `Float` |

- `deriving (Eq, Ord, Show)` is required to compare or print a data type. Without it, `A == B` is a type error.

- Integer literals have type `Num a => a`: `x + 1` and `2.5 * 2` both check, and `1` at a user `Num` type calls its `fromInt`.

- `/` is `Float` only; `div`, `mod`, `quot` and `rem` are `Int` only. `Int` + `Float` is a type error; write `toFloat n + x`.

- A constrained type nothing fixes is ambiguous: `print []` is an error, as in Haskell. A remaining `Num` type defaults to `Int`.

- A binding that is not a function (`n = 1`) gets its number type from its uses, or defaults to `Int`. A local function without a signature takes the types of its constrained variables from its uses, unlike Haskell; give it a signature to use it at two number types.

- The REPL prints a result with its `Show` instance. Without one, it prints the value's structure, or `<function> :: type`.

## Semantics

- Strict, left-to-right evaluation. `print`, `putStrLn` and `getLine ()` act when called. `a >> b` orders effects.

- Effects are in types: `a -> b` is a function that does not act by itself, `a ->> b` one that may. They are inferred, so unannotated code needs no change, and checked against signatures:

  ```haskell
  greet :: String ->> ()          -- acts
  greet name = putStrLn ("hello " ++ name)

  area :: Float -> Float          -- `->`: an error if area could act
  area r = 3.14159 * r * r

  map :: (a -> b) -> [a] -> [b]   -- acts exactly when its argument does
  ```

  All `->` arrows of one signature share an effect, so a higher-order function is as effectful as its arguments. A function that acts itself and calls a function argument writes that argument with `->>`: `withEach :: (a ->> ()) -> [a] ->> ()`. A data field or class method written with `->` holds only pure functions. To print from pure code while debugging, use `trace :: String -> a -> a`, which writes to standard error and is pure in its type.

- A `do` block runs its statements in order:

  ```haskell
  main = do
    line <- getLine ()          -- runs here, binds line
    let n = readInt line        -- pure, so lazy: computed where n is first used
    print (n * 2)
    putStrLn "done"
  ```

  `p <- e` evaluates `e` on its line and matches it against `p`. It works on any expression, not only effects: there is no `IO` type. A `let` statement that acts runs on its line; a pure one is lazy. Recursion in the last statement is a tail call.

- Top-level values must be pure, except `main`, whose evaluation is the program: `early = print 1` is an error. Write `act () = do ...`, and call it.

- Top-level values are evaluated in dependency order, with `main` last.

- Recursive bindings must be functions; `xs = 1 : xs` is rejected.

- Prelude functions, methods, types and constructors cannot be redefined at top level: `lookup k xs = ...` and `data Maybe a = ...` are errors. Use another name, or a type namespace (`Shape.lookup`). Local bindings and parameters may still reuse a prelude name.

- Local value bindings in `let` and `where` are evaluated on first use, at most once. `f x | x == 0 = 0 | otherwise = y where y = div 1 x` returns 0 for `x == 0`. A binding that can act must be a `let` statement in `do`, where it runs on its line; in `where` or `let ... in` it is an error, since a lazy effect would run at some later use, or under the wrong guard. Bind it with `<-`, or make it a function: `where rest () = print n >> loop (n - 1)`. "Can act" means its effect is `->>`, or the effect of an argument of a function around it.

- Constructor fields are strict: `let (a, b) = (head [], 1) in b` fails.

- A `case` or function that does not cover every constructor gets a compile-time warning naming an unmatched value. A guarded alternative counts as covering only if one of its guards is `otherwise` or `True`.

- Tail calls use constant stack in the interpreter. Compiled code turns self tail calls into loops.

- Deep non-tail recursion has a 4 GiB stack. Overflow is a runtime error.

## Known limitations

- `.` without spaces is field access: `f.g` is no longer composition. Write `f . g`.

- A numeric literal pattern (`f 0 = ...`) works only at `Int` and `Float`; at another `Num` type it is a runtime error.

- `String` is a packed type, not `[Char]` as in Haskell. `++`, `length`, `show`, `words`, `lines`, `unwords` and `unlines` work on it; for list functions, convert with `unpack :: String -> [Char]` and `pack :: [Char] -> String`. A string literal in a pattern matches an equal `String`.

- `Int` is 64 bits and wraps on overflow. `Char` is a byte; strings are not UTF-8 aware.

- The interpreter is about 6x slower than compiled code on allocation-heavy loops.

## Documentation

| File | Contents |
|-|-|
| [docs/dev/design.md](docs/dev/design.md) | language decisions and the alternatives rejected |
| [docs/dev/architecture.md](docs/dev/architecture.md) | compiler passes, runtime, GC, backends |
| [docs/dev/roadmap.md](docs/dev/roadmap.md) | planned releases |
| [bench/README.md](bench/README.md) | benchmarks against C and the results history |
| [examples/](examples/) | complete programs; `make test` checks their output |
| [TODO.md](TODO.md) | next steps |
| [CHANGELOG.md](CHANGELOG.md) | changes by release |
