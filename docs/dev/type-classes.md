# Type classes: design note

Status: proposal for 0.4, not implemented. Records are in [records.md](records.md); the 0.1 decisions this replaces are in [design.md](design.md).

## Problem

Three 0.1 mechanisms stand in for type classes:

- The `num` flag on type variables. `+ - *` work over `Int` and `Float` only, and users cannot add number types.
- Structural `==`, `<` and `show`. They accept every type, and fail on functions only at runtime.
- Type descriptors for `show` (`show_desc`). They are correct only where the call site's type is monomorphic. A literal in polymorphic code still prints as an `Int`.

There is no user-defined overloading: no custom `Show`, no `Monoid`, no `Functor`.

## Scope

Phase 1 implements Haskell 2010 classes over types of kind `*`:

- single-parameter classes, superclasses, default methods;
- instances with contexts (`instance Show a => Show [a]`);
- `deriving (Eq, Ord, Show)`;
- the classes `Eq`, `Ord`, `Show` and `Num` in the prelude;
- defaulting `Num` variables to `Int`, then `Float`.

Excluded: multi-parameter classes, functional dependencies, overlapping or orphan-instance rules beyond "one instance per class and type constructor", and typed `IO`. Higher-kinded classes and existential types are phases 2 and 3.

## Strategy: dictionary passing

| Option | Mechanism | Rejected because |
|-|-|-|
| Dictionary passing (chosen) | Each constraint becomes an extra argument: a constructor value holding the methods | -- |
| Monomorphization | Copy each overloaded function per instance type | The REPL interpreter would recompile per instantiation; polymorphic recursion, which signatures already permit, cannot be monomorphized |
| Runtime tags (extend 0.1) | Methods dispatch on the argument's runtime tag | Cannot dispatch on a result type (`fromInt`, `mempty`, `read`); `[]` has no element to inspect |

Dictionary passing changes the checker only. `resolve.c`, `interp.c`, `cgen.c` and `rt.c` see ordinary lambdas, applications and constructors. This is the approach of Wadler and Blott ([POPL 1989](https://dl.acm.org/doi/10.1145/75277.75283)) and of GHC. A known dictionary can later be specialized away in `cgen.c`.

## Checker changes (`check.c`)

1. **Predicates on type variables.** Replace `bool num` in `Type` with a set of class ids. Binding a variable to a variable unions the sets. Binding it to `T args` requires an instance `C T` for each class `C`, and the instance context adds predicates on `args` (context reduction). A missing instance is an error such as `no instance Show (a -> b)`. This generalizes the existing `Num` check in `bind_var`.

2. **Generalization.** A quantified variable keeps its classes, which gives schemes `forall a. Eq a => ...`. `gen` already does this for `num` through `GENERIC` plus the flag.

3. **Ambiguity and defaulting.** A constrained variable that is neither in the binding's type nor fixed by the end of the chunk is ambiguous. If one of its classes is `Num`, try `Int`, then `Float`, as in Haskell 2010 section 4.3.4 with `default (Int, Float)`. Otherwise report an ambiguity error. This replaces `default_nums`.

4. **Monomorphism restriction.** Keep it, extended from `Num` to all classes. A non-lambda binding then never takes dictionary parameters, so a value is not recomputed per use.

5. **Signatures.** `parse_sig` accepts any class in the context; today it rejects everything but `Num` (`parse.c:386`). A rigid variable's classes must cover the inferred ones. The existing error `no Num instance for rigid type variable` generalizes.

6. **Elaboration.** A new pass after inference inserts dictionaries:
   - A binding whose scheme has predicates gets one dictionary parameter per predicate.
   - Each use of an overloaded name records its instantiation, as `E_VAR.ty` already does for `show`. The pass applies it to dictionary expressions. A dictionary expression is either a dictionary parameter in scope, or an instance applied to argument dictionaries: `showList showInt`.

Inference and elaboration are the bulk of the work. My estimate is 400-600 lines of `check.c`, against 1127 today. That figure is not measured.

## Runtime representation

- A dictionary is a `K_CON` with one field per method, then one field per superclass dictionary. `Ord`'s dictionary holds an `Eq` dictionary. No new object kind.
- A method name is a selector: field `i` of its dictionary argument.
- An instance without a context is a top-level constant. An instance with a context is a function from dictionaries to a dictionary, so `show [[1.5]]` builds `Show [Float]`, then `Show [[Float]]`, at each call. Cache these only if benchmarks show the allocation matters.

## Prelude and existing primitives

- **Num.** Methods: `+`, `-`, `*`, `negate`, `abs`, `fromInt`. A literal in overloaded code becomes `fromInt d n`, which fixes the polymorphic-literal limitation. A literal at a known type stays a constant, as `resolve.c` already does for `Float`. `/` stays `Float`-only and `div` stays `Int`-only, so no `Fractional` or `Integral` yet.
- **Eq, Ord.** Instances for `Int`, `Float`, `Char` call the existing primitives. Derived instances call `fk_compare`. Structural comparison is exact once the types match, so derived `Eq` and `Ord` need no generated code.
- **Show.** A derived instance must use its fields' dictionaries, so that `Maybe Float` prints `Just 1.0` inside polymorphic code. Structural `show` cannot do that. The checker therefore generates `showsPrec` clauses per constructor, as Haskell does. `show_desc` and `desc_field` (about 100 lines) are then deleted.
- **deriving.** Today it is parsed and ignored; it becomes meaningful.

## Performance

Overloaded prelude functions (`sum`, `elem`, `sort`, `maximum`) currently call primitives directly. With dictionaries they load a field and call it through `fk_apply`. Expect regressions in the `lists`, `sort` and `strings` benchmarks; measure them with `bench/bench.py report`. The planned mitigation is in `cgen.c`: when a selector is applied to a constant dictionary, call the method directly. GHC's `SPECIALISE` does the same at the source level.

## Later phases

- **Phase 2: kinds.** `Functor` and `Foldable` abstract over `f` in `f a`. `TyDef` records only an arity, so data declarations need kind inference first.
- **Phase 3: existential constructors.** `data Shape = forall a. Shape a => MkShape a` stores a dictionary as a hidden constructor field, and a match brings it into scope. This gives dynamic dispatch and heterogeneous lists without subtyping ([GHC docs](https://downloads.haskell.org/ghc/latest/docs/users_guide/exts/existential_quantification.html)). Records of closures are a cheaper route to objects; see [records.md](records.md#objects).
- **Typed IO and monads** need phase 2. This stays an open question in [roadmap.md](roadmap.md).

Subtyping is excluded. Hindley-Milner inference with subtyping loses principal types unless the inference algorithm changes, as in MLsub ([Dolan and Mycroft, POPL 2017](https://dl.acm.org/doi/10.1145/3009837.3009882)).

## Open questions

- **Implicit deriving.** Without it, `A == B` on `data T = A | B` becomes a type error. Every 0.1 program that compares or prints a user type without `deriving` breaks. The options are to require `deriving`, as Haskell does, or to derive `Eq`, `Ord` and `Show` for every `data` declaration that has no explicit instance. I recommend requiring `deriving`, with the change listed in the CHANGELOG.
- **REPL output for types without Show.** `id` prints `<function>` today. Should the REPL fall back to structural `show`, or report the missing instance?
- **Ord's method set.** Haskell's `Ord` has `compare` plus five derived methods. A dictionary with only `compare` is smaller. A full one lets an instance define `<` directly.
- **Instances in the REPL.** Each instance must be unique per class and type. A failed input must undo its instances through the undo log, as it already does for globals and fixities.

The citations are from memory and were not checked in this session.
