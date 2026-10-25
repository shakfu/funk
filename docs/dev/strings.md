# Packed strings: design note

Status: option A implemented (unreleased). [performance-first.md](performance-first.md) decided that strings become a packed type. Breaking `String = [Char]` was accepted because funk has no releases and no users yet.

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

## Decision

This note first recommended deferring both A and C: the measurement shows packing does not close the benchmark's gap, and A breaks the Haskell rule that `String = [Char]`. A was chosen instead, because with no releases and no users the break costs nothing yet, and text performance matters for real programs. The benchmark's C version was rewritten to use packed strings, so the metric stays fair.

## As implemented

- `K_STR` objects hold the length in the header and the bytes after it, NUL-terminated. The collector does not scan them. A literal is a static object made on its first evaluation, once per literal, in both backends.

- `++` and `length` are methods of `Append` and `Sized`, with instances for `[a]` and `String`. At `String`, both compile to primitives (`primAppendString`, `primLengthString`), as `==` and `show` do.

- `words`, `lines`, `unwords`, `unlines` and `concatStrings` are primitives: one pass, one allocation for joins.

- The class default `showList` joins with `concatStrings`; joining with `++` would copy the growing tail on every step.

- Tests that used strings as lists now call `unpack`: 5 places in the repo.

- `bench/progs/strings.c` was rewritten with `char` buffers, so the benchmark compares packed with packed. `strings` is 1.6x C; it was 3.3x C when both versions used lists. The interpreter went from 1.32 s to 0.12 s on it.
