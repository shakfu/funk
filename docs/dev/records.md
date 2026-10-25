# Records: design note

Status: proposal for 0.4, not implemented. Type classes are in [type-classes.md](type-classes.md).

## Problem

Constructor fields are positional. `data Config = Config Int Int Bool String` needs a pattern-matching helper per field to read it, and every field rebuilt to change one.

## Options

| Option | Field names | Type system | Runtime |
|-|-|-|-|
| A. Haskell 98 field labels | Global selector functions; one type per name per module | No change: desugars to selectors and `case` | `K_CON`, fields by position |
| B. Type-scoped fields (chosen) | Belong to their type; `p.x` resolved from the type of `p` | Deferred field constraints in `check.c` | As A |
| C. Row-polymorphic structural records | Structural: `.x :: { x : a \| r } -> a` | Row variables and new unification rules ([Leijen 2005](https://www.microsoft.com/en-us/research/publication/extensible-records-with-scoped-labels/)) | Offsets unknown in polymorphic code: runtime lookup, or offsets passed as evidence |
| D. A `HasField` class, as in GHC | `p.x` via `HasField "x" r a` | Needs multi-parameter classes and type-level strings | Dictionaries |

B is chosen. Two types can share a field name, and a field name never clashes with a function, so `data Item = Item { length :: Int }` coexists with the prelude's `length`. A needs no checker change, but its global names collide, a known problem in Haskell 98.

B's cost: a field access whose record type is unknown where it is generalized is an error. `getX p = p.x` needs a signature. C and D both infer `getX` without one. C makes records structural, not nominal. D needs type classes beyond phase 1. Either remains possible later, because B's syntax is a subset of theirs.

## Semantics

- **Declaration.** `data Shape = Circle { radius :: Float } | Rect { w, h :: Float }`. Field order gives the position. One constructor cannot mix labelled and positional fields. Constructors of one type may share a field if its type is the same in each.
- **Scope.** Field names are in their type's namespace, not the global one. They are not functions and do not shadow or clash with variables.
- **Access.** `p.x`, and `(.x)` as a function; see [Dot access](#dot-access). On a type with several constructors, a field missing from the value's constructor is a runtime error: `(Rect 1.0 2.0).radius`.
- **Construction.** `Rect { h = 2.0, w = 1.0 }` accepts any order. The constructor names the type, so no inference is needed. A missing field is a compile error. Haskell accepts it with a warning and stores a bottom value; under strict evaluation that value would fail on construction anyway.
- **Update.** `s { w = 3.0 }` copies `s` with new fields. The type of `s` is resolved like `s.w`. It desugars to a `case` over the constructors that have every named field; the others are a runtime error.
- **Patterns.** `Rect { w = x }` matches `Rect` and binds `x`. `Rect {}` matches any `Rect`. Both desugar to positional patterns with wildcards.
- **Positional use** keeps working: `Rect 1.0 2.0`, and `Rect a b` as a pattern.
- **Show** prints `Rect {w = 1.0, h = 2.0}`, as Haskell's derived `Show` does.
- **Excluded for now:** puns and wildcards (`Rect {..}`), nested update (`p { pos.x = 1 }`).

## Dot access

The lexer separates field access from composition by whitespace, as GHC's `OverloadedRecordDot` does ([GHC proposal 282](https://github.com/ghc-proposals/ghc-proposals/blob/master/proposals/0282-record-dot-syntax.rst)):

| Source | Meaning |
|-|-|
| `p.x`, `(f p).x` | field access: no space on either side of `.`, a lowercase name after it |
| `p.pos.x` | field `x` of field `pos` of `p` |
| `(.x)` | `\r -> r.x`, so `map (.x) ps` |
| `f . g`, `(. g)` | composition, as today |
| `f.g` | field access; was composition in 0.1 |

- **Precedence.** Field access binds tighter than application: `f p.x` is `f (p.x)`.
- **Left side.** An identifier, `)` or `]` with no space before the `.`. A type name on the left (`Shape.area`) is a qualified name; see [Type namespaces](#type-namespaces).
- **Right side.** A lowercase field name. `1.5` stays a float literal, since the lexer reads numbers first.
- **Breaking change.** `f.g` without spaces changes meaning. The prelude, tests and benchmarks have no such use. 0.1 code that has one gets an error.
- **Pipes** still work: `ps |> map (.x)`.

## Resolution

`p.x` cannot be looked up while it is inferred. In `map (.x) ps`, the argument `(.x)` is inferred before `ps` fixes its type. The checker therefore records a deferred constraint "`t` has field `x` of type `a`":

1. `p.x` gives `p` a fresh type `t` and the result a fresh type `a`. It appends `(t, x, a, location)` to a pending list.
2. Before each binding group is generalized, the checker retries every pending entry. When `t` resolves to `T args`, it finds field `x` of `T`, instantiates the field's type with `args`, unifies it with `a`, and records the field's position for the backends. One success can fix the type of another entry (`p.pos.x`), so it repeats until nothing changes.
3. An entry whose `t` is still a variable is either:
   - an error, if `t` would be generalized in this group: `cannot infer the record type for .x; add a type signature`;
   - carried to the enclosing group, if `t` belongs to an outer level.
4. If `T` has no field `x`: `type Shape has no field radius2`.

A signature is unified before the group is generalized, so `getX :: Point -> Float; getX p = p.x` resolves. `let f q = q.x in f p` fails without a signature for `f`, because `q`'s type would be generalized.

Step 3 has no fallback that picks the only type with field `x`. Such a fallback would make a program stop compiling when an unrelated type later adds a field `x`, which in the REPL can be any later input.

Once resolved, `p.x` becomes a `case` on `p` with one alternative per constructor that has `x`. The backends see only `case`.

## Type namespaces

A type is a namespace for its fields and for associated functions:

```haskell
data Shape = Circle { radius :: Float } | Rect { w, h :: Float }

Shape.area :: Shape -> Float
Shape.area (Circle r) = 3.14159 * r * r
Shape.area (Rect w h) = w * h

Shape.scale :: Shape -> Float -> Shape
Shape.scale (Circle r) k = Circle (r * k)
Shape.scale (Rect w h) k = Rect (w * k) (h * k)
```

- **Definition.** A top-level clause whose name is `Type.name` adds `name` to `Type`'s namespace. `Type` must be declared in the same module. A field and an associated function of one type cannot share a name.
- **Qualified use.** `Shape.area` and `Shape.radius` are ordinary functions, so `map Shape.area shapes` needs no inference.
- **Method call.** `s.name` looks up a field `name` in `s`'s type, then an associated function, and applies the function to `s`: `s.area` is `Shape.area s`, and `s.scale 2.0` is `Shape.scale s 2.0`. Resolution uses the pending check above. Calls are static; there is no runtime dispatch.
- **Argument order.** Method calls need the receiver first. Pipelines and the prelude put data last (`s |> scale 2.0`). Associated functions take the receiver first; ordinary functions keep data last.
- **Lexing.** A conid, `.` and a varid with no spaces form one qualified-name token.

## Modules

Modules are not designed yet (see [roadmap.md](roadmap.md)). Reserved syntax:

- Module names are lowercase and use `.` as types do: `geometry.pi`, `geometry.Shape`, `geometry.Shape.area`. Case already decides the meaning: uppercase after `.` is a type or constructor, lowercase is a value, field or function.
- `geometry.pi` is a module member when `geometry` names a module, and field access otherwise. A local variable with the name of an imported module is an error.
- Constants are lowercase, `geometry.pi`; `geometry.PI` would be a constructor.
- `::` is not used for paths, because it is the annotation operator: `geometry::PI` already parses as `geometry` annotated with type `PI`, and `(x::Int)` is common Haskell style.

## Implementation

- **Lexer.** A `.` with no space before it, after an identifier, `)` or `]`, and directly followed by a lowercase letter, becomes a new token `T_FIELD` carrying the name. `(.` directly followed by a lowercase name and `)` becomes a field section.
- **Parser.** `T_FIELD` is a postfix operator in `parse_aexp`, applied before function application. `{` after a constructor in a `data` declaration starts a field list. `{` after an atomic expression or constructor pattern starts a construction, update or record pattern. This is unambiguous: the parser otherwise accepts `{` only where a layout keyword opens a block. New nodes: `E_FIELD`, `E_RECORD` and a record pattern.
- **Checker.** `register_datas` stores field names and types per constructor in `ConDef`. Construction and patterns are lowered after `register_datas`, since a `data` declaration may appear after its use. Field access and update are lowered after resolution (above). In the REPL, a failed input truncates the pending list with the other tables.
- **Runtime.** `fk_register_con` also takes field names (NULL for positional), so structural `show` and `cgen.c`'s constructor table can print record syntax.

My estimate is 300-400 lines for fields, mostly in `parse.c` and `check.c`, about 100 more than option A. Associated functions and method calls add about 100. Neither figure is measured.

## Objects

Records of closures give objects with hidden state and open recursion, without new type system features:

```haskell
data Counter = Counter { get :: () -> Int, incr :: () -> Counter }

counter n = Counter { get = \_ -> n, incr = \_ -> counter (n + 1) }

main = print (((counter 0).incr ()).get ())
```

Each value of type `Counter` is an object. Its state lives in the closures, and `incr` returns the next object. Different implementations share one type, so a list of them is heterogeneous. Existential constructors (phase 3 in [type-classes.md](type-classes.md)) give the same with class dispatch.

## Order

Records come before type classes:

- they are smaller;
- they are independent of the rest of 0.4;
- derived `Show` in phase 1 of type classes reuses their field names.
