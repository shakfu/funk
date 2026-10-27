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
make test ARGS="-R gcdeep"     # the tests whose names match; ARGS go to ctest (-L ffi for a label)
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
funk build prog.fk -O3 -lsqlite3   # C optimization level; libraries to link (-I, -L, -l)
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

Bitwise operations on `Int` take `Data.Bits`' names and fixities: `.&.` (infixl 7), `xor` (6), `.|.` (5), `complement`, and `shiftL` and `shiftR` (8, usually written in backticks). A shift by 64 or more gives 0, or -1 for `shiftR` of a negative number; a negative shift is an error.

Other built-in operators have Haskell's fixities. User-defined operators default to `infixl 9`. A top-level declaration sets the fixity of operators bound in the same module:

```haskell
infixr 5 +++
infixl 7 `times`
```

C functions are called through foreign imports, with Haskell's syntax. The entity string names the header and the C function; the funk type picks the conversions. `->>` marks an import that acts; `->` is the importer's promise that it does not:

```haskell
foreign library "m"   -- links -lm; the interpreter loads it too
foreign import ccall "math.h cbrt" cbrt :: Float -> Float
foreign import ccall "stdlib.h srand" srand :: CUInt ->> ()
foreign import ccall "math.h M_PI" piC :: Float   -- a C value, read once at startup
```

The declared types must match the C declaration: a conversion that could change a value, or drop a `const`, is a compile error, while widening is allowed. `Int` is `int64_t`, `Float` is `double`, `Char` is `char` and `Bool` is `bool`. `CInt`, `CUInt`, `CLong`, `CULong`, `CLLong`, `CSize` and `CFloat` are C's `int`, `unsigned`, `long`, `unsigned long`, `long long`, `size_t` and `float`. Their arithmetic wraps to the C width, and `fromIntegral` converts between integer types (`toCFloat` and `fromCFloat` for `CFloat`). A result may be `()`, and `()` alone as the argument calls a C function without parameters.

`String` passes as `const char *`; a NUL inside it, or a NULL result, is a run-time error. `Ptr a` is a C pointer, and `data Db` without constructors declares a type for `Ptr Db`. `alloca`, `peek` and `poke` (class `Storable`), `mallocBytes`, `free`, `nullPtr`, `castPtr`, `plusPtr` and `peekCString` work as in Haskell; [tests/ffi/sqlite.fk](tests/ffi/sqlite.fk) uses sqlite3 through them.

C calls funk back through a function pointer whose context argument is a `StablePtr` to a funk closure. A value import may be any C expression, so `sizeof` and `offsetof` give struct layouts for `pokeByteOff`:

```haskell
foreign import ccall "callback" onPort :: FunPtr (Ptr () -> Ptr Port ->> ())  -- runs at once, on funk's thread
foreign import ccall "queue" onMidi :: FunPtr (Ptr () -> CLLong -> CStringLen -> ())  -- any thread; waitEvent runs it
foreign import ccall "fkt.h offsetof(fkt_config, count)" offCount :: CSize
```

[examples/ffi/libremidi.fk](examples/ffi/libremidi.fk) sends and receives MIDI through libremidi this way. Queued calls go through a fixed ring of 4096 events, without `malloc` or locks; a full ring drops events, and `droppedEvents ()` counts them.

For a C caller with a deadline, such as an audio callback, `foreign export ccall "fk_process" process :: Ptr () -> Ptr CFloat -> CInt ->> ()` makes `fk_process` a C function that any thread may call. The compiler rejects an export whose compiled code could allocate or fail: it may use C scalars and pointers, `peek` and `poke`, loops and calls to such functions, but no lists, strings or closures, no division by a variable and no `case` that misses a value. `foreign import ccall "fkt.h &fk_process"` gives its address. `Array a` and `Ring a` hold `Storable` values in C memory, freed explicitly: an array with checked indices (`readArray`) or wrapping ones (`readArrayWrap`, which an export may use), and a single-producer, single-consumer ring for messages between funk's thread and a callback, either way (`ringPush`; `ringCount`, `ringFront`, `ringDrop`). `atomicPeek`, `atomicPoke`, `atomicAdd` and `atomicCompareSwap` share single values. `foreign struct` declares a C struct, from a header or laid out as C would, which may hold others by value; its fields are read with `readField` and written with `writeField`. `truncateSat` converts a `Float` to an `Int` without failing. Exports need `funk build`; [examples/ffi/portaudio.fk](examples/ffi/portaudio.fk) is a PortAudio wavetable synth, whose callback took at most 5 us of its 1.33 ms buffer while funk's thread collected garbage.

Compiled code calls the C function directly. The interpreter compiles the same glue with `$CC` once and caches it in `$XDG_CACHE_HOME/funk/ffi` (default `~/.cache/funk/ffi`). `funk run`, `repl` and `build` take `-I`, `-L` and `-l` for headers and libraries outside the default paths. See [docs/dev/ffi.md](docs/dev/ffi.md).

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

- `String` is a packed type, not `[Char]` as in Haskell. `++`, `length`, `show`, `words`, `lines`, `unwords` and `unlines` work on it, and `indexString s i` reads one character; `writeArrayString arr i s` copies a string's bytes into an `Array CChar`; `putArray arr i n` writes `n` elements of an array to standard output as bytes, and `getArray arr i n` reads up to `n` from standard input, returning how many (0 at its end); `copyArray src i dst j n` copies elements as `memmove` does; for list functions, convert with `unpack :: String -> [Char]` and `pack :: [Char] -> String`. A string literal in a pattern matches an equal `String`. `isEOF ()` tells whether standard input has ended, for reading it line by line with `getLine ()`; `getContents ()` reads all of what is left, at once.
- `IntTable` is a mutable hash table from `Int` to `Int` in C memory, in the prelude: `newIntTable n`, `insertIntTable t k v`, `addIntTable t k d` (adds to the value, a new key's being 0), `lookupIntTable t k :: Maybe Int`, `intTableList t` and `freeIntTable t`.

- `Int` is 64 bits and wraps on overflow. `Char` is a byte; strings are not UTF-8 aware.

- The interpreter is about 6x slower than compiled code on allocation-heavy loops.

- The collector pauses the program for at most about 0.5 ms at a time, at any heap size and stack depth: 0.47 ms with a million frames on the stack. The heap is up to 70% larger than with marking in one pause, since objects allocated during a cycle live until the next. `FUNK_GCSTATS=1` prints each pause ([docs/dev/realtime.md](docs/dev/realtime.md)).

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
