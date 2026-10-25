# Records: design note

Status: proposal for 0.4, not implemented. Type classes are in [type-classes.md](type-classes.md).

## Problem

Constructor fields are positional. `data Config = Config Int Int Bool String` needs a pattern-matching helper per field to read it, and every field rebuilt to change one.

## Options

| Option | Syntax | Type system | Runtime |
|-|-|-|-|
| A. Haskell 98 field labels (chosen) | `data P = P { x, y :: Float }`; `x p`; `P { x = 1.0, y = 2.0 }`; `p { x = 3.0 }` | No change: desugars to constructors, selector functions and `case` | `K_CON`, fields by position |
| B. Row-polymorphic structural records | `{ x = 1, y = 2 }`; `.x :: { x : a \| r } -> a` | Row variables in `Type` and new unification rules ([Leijen 2005](https://www.microsoft.com/en-us/research/publication/extensible-records-with-scoped-labels/)) | Field offsets are unknown in polymorphic code: look up labels at runtime, or pass offsets as evidence, like dictionaries |
| C. A `HasField` class, as in GHC | `p.x` resolves via `HasField "x" p a` | Needs multi-parameter classes and type-level strings | Dictionaries |

A is chosen. It keeps Haskell syntax, changes neither inference nor the runtime, and does not depend on type classes.

A's cost is that a field name is a global function. Two types in one module cannot share a field name, as in Haskell 98. B removes that limit and gives structural typing, which covers much of what OO subtyping is used for. It is the alternative to revisit if funk moves away from Haskell syntax. Its checker cost is close to that of type classes.

## Semantics

- **Declaration.** `data Shape = Circle { radius :: Float } | Rect { w, h :: Float }`. Field order gives the position. One constructor cannot mix labelled and positional fields.
- **Selectors.** Each field generates a binding, `radius :: Shape -> Float`. It is partial when the type has several constructors: `radius (Rect 1.0 2.0)` is a runtime error. Generated selectors are excluded from the exhaustiveness warning; GHC also does not warn by default. GHC's `-Wpartial-fields` warns at the declaration instead, an option for later.
- **Construction.** `Rect { h = 2.0, w = 1.0 }` accepts any order. A missing field is a compile error. Haskell accepts it with a warning and stores a bottom value; under strict evaluation that value would fail on construction anyway.
- **Update.** `s { w = 3.0 }` copies `s` with new fields. It desugars to a `case` over the constructors that have every named field; the others are a runtime error. All fields must belong to one type.
- **Patterns.** `Rect { w = x }` matches `Rect` and binds `x`. `Rect {}` matches any `Rect`. Both desugar to positional patterns with wildcards.
- **Positional use** keeps working: `Rect 1.0 2.0`, and `Rect a b` as a pattern.
- **Show** prints `Rect {w = 1.0, h = 2.0}`, as Haskell's derived `Show` does.
- **Excluded for now:** puns and wildcards (`Rect {..}`), and dot access. In `p.x` the dot is already composition: `f.g` without spaces means `f . g`. Write `x p` or `p |> x`. For nested fields, `p |> outer |> inner` applies, and `outer >>> inner` composes a reusable accessor.

## Implementation

- **Parser.** `{` after a constructor in a `data` declaration starts a field list. `{` after an atomic expression or constructor pattern starts a construction, update or record pattern. This is unambiguous: the parser otherwise accepts `{` only where a layout keyword opens a block.
- **Desugaring happens in `check.c`, not the parser.** Construction needs each constructor's field order. That is known only after `register_datas`, and a `data` declaration may appear after its use. The parser emits a new expression kind, `E_RECORD`, and a record pattern. `check.c` lowers both after `register_datas` and before `scc`.
- **Runtime.** `fk_register_con` also takes field names (NULL for positional), so structural `show` and `cgen.c`'s constructor table can print record syntax. With type classes, derived `Show` uses the same names.

My estimate is 200-250 lines, mostly in `parse.c` and `check.c`. It is not measured.

## Objects

Records of closures give objects with hidden state and open recursion, without new type system features:

```haskell
data Counter = Counter { get :: () -> Int, incr :: () -> Counter }

counter n = Counter { get = \_ -> n, incr = \_ -> counter (n + 1) }
```

Each value of type `Counter` is an object. Its state lives in the closures, and `incr` returns the next object. Different implementations share one type, so a list of them is heterogeneous. Existential constructors (phase 3 in [type-classes.md](type-classes.md)) give the same with class dispatch.

## Order

Records come before type classes:

- they are about a third of the size;
- they are independent of the rest of 0.4;
- derived `Show` in phase 1 of type classes reuses their field names.
