# Packed strings: design note

Status: proposal, needs a decision. [performance-first.md](performance-first.md) decided that strings become a packed type. This note shows what that means for the language, and a measurement that bears on whether to do it now.

## What the metric says

`strings` is 3.3x slower than C. The C program in `bench/progs/strings.c` builds the same `[Char]` linked lists, so the gap is collection and generated-code overhead, not packing. After the latest changes, the generated `strings` program contains no unknown calls (`fk_apply`). Collection is about 35% of its time, the same as in `lists` and `sort` ([memory.md](memory.md#after-trmc)).

A packed representation would speed up `strings` by more than the gap, by changing the work rather than the overhead. The C baseline would then be a list program racing a packed one. Packed strings are therefore not needed to reach the target. Their value is for real programs that process text.

## Options

| | Option | Language | Cost to other code |
|-|-|-|-|
| A | `String` is a distinct packed type, as `Data.Text` is in Haskell | `"abc" :: String`, not `[Char]`; `(c:cs)` patterns and list functions no longer apply to strings | none |
| B | `[Char]` keeps its type, with a packed representation the runtime treats as a list | unchanged | every pattern match on a list must also accept the packed form, and taking a tail allocates a slice: slower list code everywhere |
| C | Keep `[Char]` lists; make string functions primitives (`words`, `unwords`, `lines`, `++`, `show`) | unchanged | none; the gain is limited to those functions |

B costs every list operation in exchange for strings, which goes against the performance goal. A or C remain.

### A in detail

- **Types.** `String` is a builtin type. Literals have type `String`. `[Char]` remains a list type, with `pack :: [Char] -> String` and `unpack :: String -> [Char]` between them.
- **Representation.** A heap object holding a length and bytes. A literal is a static object, allocated once.
- **Overloading.** `++` and `length` become methods of classes with instances for `[a]` and `String`. Single-parameter classes over the type itself (`[a]`, `String`) suffice; no higher-kinded classes are needed.
- **Prelude.** `words`, `unwords`, `lines`, `unlines`, `show`, `putStr`, `putStrLn`, `getLine`, `readInt`, `readFloat` and `error` move to `String`, implemented as primitives.
- **Patterns.** A string literal pattern compares by equality. `(c:cs)` on a `String` is a type error; write `case unpack s of (c:cs) -> ...`.
- **Breaks.** Code that treats strings as lists: `map toUpper s`, `filter isDigit s`, `s !! 0`, `(c:cs)` patterns on strings. In this repo, the prelude's string functions, and the tests that use `words`, `++` or pattern-match strings.
- **Size.** About 400 lines across the checker, runtime, prelude and tests. An estimate.

### C in detail

The prelude's string functions become C primitives over `[Char]` lists. They keep their types and semantics. They remove per-character calls and allocation in those functions, but not the cost of `[Char]` itself.

## Recommendation

Revisit the decision. The measurement shows packed strings do not serve the metric, and A breaks the Haskell rule that `String = [Char]`. If text performance matters beyond the benchmark, A is the right design and C is its cheap partial version. If not, defer both. A, if chosen, should land with a text-processing benchmark whose C version uses a packed string, so the metric measures it fairly.

## Decision needed

1. A (distinct packed `String`), C (primitives over `[Char]`), or neither for now?
2. If A: is breaking `String = [Char]` acceptable?
