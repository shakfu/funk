# Haskell compatibility: gap note

Status: survey, 2026-10-26, of funk 0.2.0. It lists what stops Haskell source from running in funk, and what each typed-effects option changes ([typed-effects.md](typed-effects.md)). The answer it gives: no effect option makes funk source-compatible with Haskell. `IO` typing is one gap among about fifteen, and the deepest one, evaluation order, is fixed by design.

## Framing

funk has Haskell's syntax and an ML's semantics: strict evaluation, with lazy `let` and `where` as the one exception ([design.md](design.md), [typed-effects.md](typed-effects.md#why-let-is-lazy)). A small Haskell program that does not depend on laziness ports with edits. Typical Haskell programs and libraries do not, because they use modules, laziness, `newtype`, the numeric classes and `String = [Char]`.

## What works

Each was run through `funk -e` or as a file:

- `case` alternatives with guards; `where` over several guards;

- tuple and lambda patterns, `let (a, b) = ...`;

- sections, `2 ^ 10`, `negate (-3)`;

- arithmetic ranges `[1 .. 10]`, `[1, 3 .. 9]`;

- `foldr`, `filter`, `reverse`, `concat`, `replicate`, `lookup`, `fst`, `mapM_`, `print`;

- `toUpper` on a `Char`;

- `show` on data, lists, `Maybe` and `Float`;

- `deriving (Eq, Ord, Show)`.

## What does not

| Haskell | funk 0.2.0 | Kind | Fixed by B1 or B2 |
|-|-|-|-|
| `module Main where`, `import Data.List` | parse error: no modules | module system | no |
| `[x * 2 \| x <- xs]` | parse error: no list comprehensions | syntax | no |
| `take 3 [1..]` | parse error: no infinite ranges | laziness | no |
| `let xs = 1 : map (* 2) xs` | rejected: "defined recursively but is not a function (evaluation is strict)" | laziness | no |
| `newtype Age = Age Int` | rejected: no `newtype` | syntax | no |
| `type Name = String` | rejected: no type synonyms | type system | no |
| `fromIntegral`, `Integral`, `Fractional`, `Integer` | not in scope: `Num` is the only arithmetic class, and `Int` the only integer type; float literals use `FromFloat` | type classes | no |
| `succ`, `minBound`, `maxBound`, `deriving (Enum, Bounded)` | not in scope; deriving is limited to `Eq`, `Ord` and `Show` | type classes | no |
| `read "42" :: Int` | not in scope: funk has `readInt` and `readFloat` | prelude | no |
| `divMod`, `zipWith3`, `seq`, `interact` | not in scope | prelude | no |
| `map toUpper s` on a `String` | type error: `String` is packed, not `[Char]`; `unpack` converts | a 0.2 decision | no |
| `name st` for a record field | fields are scoped to their type; funk writes `st.name` | a 0.2 decision | no |
| `Functor`, `Foldable`, `Traversable` | absent: no higher-kinded classes | type classes | B2 adds kinds; the classes are separate work |
| `main :: IO ()`, `getLine :: IO String`, `return`, `>>=` | no `IO` type | effects | yes |
| `do` over `Maybe`, `State`, parsers | `do` is sequencing only | effects | B2 only |

The `name st`, `Functor` and last two rows follow from the design and the typed-effects note; the others were run.

## Laziness

The deepest difference is evaluation order, not syntax. Idiomatic Haskell relies on laziness: infinite lists, `zip [1..] xs`, `iterate`, `cycle`, knot-tied definitions, and `foldr` over large inputs. In funk the same text fails to compile or does not terminate. No effect option changes that. Closing the gap would mean lazy evaluation by default, which the performance work rests on not having ([performance-first.md](performance-first.md)). It stays out of scope.

Two narrower forms are possible without changing the default:

- **Infinite ranges as producers only.** `[n ..]` consumed by `take`, `takeWhile` or `zip` could be accepted when fusion removes the list ([fusion.md](fusion.md)). That makes a program's acceptance depend on an optimization, which is a poor rule.

- **An explicit lazy list type,** a `Stream` in the prelude. That is a library, not compatibility: Haskell code would still need rewriting.

## What the effect options change

- **B1:** Haskell code that differs from funk only in `IO` typing ports: small programs built from `getLine`, `putStrLn`, `do` and pure helpers.

- **B2:** `Monad`-generic code ports too, once kinds exist: `mapM`, `when`, `do` over `Maybe` or `State`.

- **E:** no `IO` code ports, since funk would have no `IO` type. Pure code ports as it does today.

Because `IO` is one gap among many, "Haskell compatibility" is a weaker argument for B1 than [typed-effects.md](typed-effects.md#the-options-compared) suggests. What remains of it is familiarity: Haskell readers know the `IO` style.

## Closing the gap, if it is a goal

The items that do not touch laziness, roughly by value per unit of work. Sizes are estimates.

| Item | Change | Estimate |
|-|-|-|
| Prelude names: `divMod`, `quotRem`, `zipWith3`, `zip3`, `unzip3`, `succ`, `pred`, `seq`, `interact`, `getContents` | prelude functions and a few primitives. Prelude names are reserved ([design.md](design.md#prelude-names-are-reserved)), so each addition breaks programs defining that name | 80 lines |
| `type` synonyms | expanded when a type is read (`te_type`) | 60 lines |
| `newtype` | a one-constructor `data` at first; without its run-time cost later | 30 lines |
| List comprehensions | desugared to `concatMap` and `filter`; fusion then removes the lists | 80 lines |
| `deriving (Enum, Bounded)` | generated instances, as for `Eq`, `Ord` and `Show` | 100 lines |
| `Integral`, `Fractional`, `fromIntegral`, `read` | numeric classes beside `Num`, and a `Read` class or a fixed `read` at known types | 300 lines, and a design note |
| Modules and `import` | done in 0.6.0 ([modules.md](modules.md)): `module Main where` and base imports such as `import Data.List (sortBy)` are accepted | large |

Since this survey, foreign imports added `fromIntegral` and an `Integral` class whose one method is `toInt`, not Haskell's `toInteger` and division methods ([ffi.md](ffi.md#implementation-phase-1)). Haskell's `Integral` would extend that class.

Two items are decisions taken in 0.2 and kept deliberately, not gaps to close: packed `String` ([strings.md](strings.md)) and fields scoped to their type ([records.md](records.md)). Undoing either for compatibility would cost speed or the field design.

## Questions

1. **What level of compatibility is the goal?** "Small, strict-safe Haskell programs port with edits" is close: the first five rows of the table above would cover most of what such programs hit. "Haskell libraries port" is not reachable without laziness.

2. **Does compatibility change the typed-effects decision?** If the goal is the first level, `IO` matters little, and E loses little by not having it.

These results are for funk 0.2.0 as built on 2026-10-26.
