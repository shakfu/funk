# Typed effects: design note

Status: proposal, decision needed. The status line used to say that under the performance goal ([performance-first.md](performance-first.md)) the recommendation changes from B1 to A', because B1's runner allocates per action. "Investigation" below measures that at +5% on an effect-heavy benchmark and recommends B1 again, with two corrections to its semantics. It settles the first open question in [roadmap.md](roadmap.md#open-questions). The current design is in [design.md](design.md#effects-impure-primitives-sequenced-by-).

## Problem

Effects are untyped. `print` has type `Show a => a -> ()` and prints when the call is evaluated. Three later features made the consequences visible:

- **Lazy `let` moves effects.** `let a = getLine ()` reads at the first use of `a`, not at its line. `let a = print 1 in a >> a` prints once, since the binding is memoized.

- **`do` cannot mean what it says.** `<-` accepts any expression, `x <- 1 + 2` included. A top-level `act = do ...` is a value, so it runs once, at startup. A test in this repo fell into this while `do` was being written.

- **Types hide effects.** `f :: Int -> Int` may print, read input or exit. A Haskell reader assumes it does not.

None of these is a bug in the implementation. They come from Haskell syntax over OCaml semantics, with Haskell's lazy `let` added.

## Options

| | Option | Effects in types | Checker | Breaks 0.2 code |
|-|-|-|-|-|
| A | Keep untyped effects (OCaml, F#, SML) | no | none | no |
| A' | A, plus strict `let` statements in `do` | no | none | rarely |
| A'' | A', plus purity the compiler infers, reported or checked | inferred, not in types | none; uses the existing analysis | rarely |
| B1 | `IO a` with a bind for `IO` only | yes | small | yes |
| B2 | `IO a` with a `Monad` class | yes | type classes phase 2 | yes, as B1 |
| C | Effect rows (Koka) | yes, per effect | large: row types | yes |
| D | Uniqueness types (Clean) | yes | large | yes |
| E | Effect arrows (Roc): `a -> b` pure, `a ~> b` effectful; calls stay direct | yes, one bit per arrow | medium: an effect on every arrow | rarely: effectful signatures change |

- **A** is what strict ML-family languages do, and it works there because their `let` is strict. Effect order is then the order of the source.

- **A'** removes the `do` surprise: a `let` statement in `do` would run at its line, as `<-` does. Effects stay invisible in types, top-level effects still run at startup, and `let` would mean two things.
- **A''** is A' plus the purity that `fuse.c` already infers ([fusion.md](fusion.md#semantics-when-fusion-is-exact)), made visible: a `pure` declaration the checker verifies, rejecting the program if the function can act ("A'' in detail"). Effects compose with any function, as in A', and readers and the optimizer learn some of what B1 would tell them. The guarantee is weaker than a type: inference stops at unknown calls (closures, parameters, dictionaries) and must assume they act.

- **B1** is Haskell's `IO` ([Peyton Jones and Wadler, POPL 1993](https://dl.acm.org/doi/10.1145/158511.158524)), with `bindIO :: IO a -> (a -> IO b) -> IO b` as a plain function. `do` works for `IO` only. No higher-kinded classes are needed.

- **B2** is B1 with `class Monad m` over `m`, which needs kinds. `do` then also works for `Maybe`, lists, parsers and state.

- **C** types each effect separately: `print : a -> <console> ()` ([Leijen, MSFP 2014](https://arxiv.org/abs/1406.2061)). It is the most precise, but it needs row polymorphism in the checker, about the size of type classes again. Its syntax is not Haskell's.

- **D** threads a unique `World` value: `getLine :: World -> (String, World)`. Code becomes verbose without new syntax. Rejected.

- **E** puts one bit in every function arrow: pure or effectful. There is no `IO` type and no runner; an effectful call is a direct call, as today. Roc does this with `->` and `=>` ([roc-lang.md](roc-lang.md#1-effects-in-the-arrow)). funk spells the effectful arrow `~>`, because `=>` already marks class contexts. A higher-order function is polymorphic in its argument's effect, so one `map` serves both (see "E in detail").

Recommendation: E, after a prototype of its inference; see "The options compared". Otherwise B1, with B2 decided later, on its own. B1 is source-compatible with B2, because a `do` over `IO` written for B1 means the same under `Monad IO`. If funk stays impure, choose A'': it includes A' and fixes the `do` surprise.

## B1 in detail

### Semantics

- `IO a` is an abstract builtin type. A value of type `IO a` is an action: a description of effects that yields an `a`. Evaluating it does nothing.

- The runtime runs `main :: IO ()`. Nothing else runs an action. Top-level `IO` values no longer run at startup.

- Haskell's types: `putStrLn :: String -> IO ()`, `print :: Show a => a -> IO ()`, `getLine :: IO String`, `return :: a -> IO a`, `(>>=)`, `(>>)`, `mapM_ :: (a -> IO b) -> [a] -> IO ()`.

- `do` desugars to `bindIO e (\p -> rest)` for `p <- e`, and to `bindIO e (\_ -> rest)` for `e`. A `thenIO e rest` would build `rest` before `e` runs; see "Investigation". `let` statements stay lazy. That is now harmless, because binding an action does not run it.

- `a >> b` is a special form, like `&&`: it means `bindIO a (\_ -> b)`, so `b` is built only after `a` has run.

- `error` stays pure and raises a runtime error, as in Haskell.

- `trace :: String -> a -> a` prints when evaluated, as `Debug.Trace.trace` does. It is the only way left to print from pure code, kept for debugging.

### Runtime: a trampolined runner

An action is a heap value, interpreted by a loop in `rt.c`:

```text
Pure x | Prim f | Bind m k        -- k: a funk closure a -> IO b
```

`fk_run_io` keeps an explicit continuation stack. `Bind m k` pushes `k` and continues with `m`. A finished `Pure x` pops a `k` and continues with `k x`. A recursive loop like `loop n = do { print n; loop (n - 1) }` then runs in constant C stack in both backends, whatever the association of the binds.

The alternative is `IO a = () -> a` with `bindIO m k = \u -> k (m u) u`. That needs no runner, and the interpreter's tail calls keep it flat. But compiled code calls `k` through `fk_apply`, so every iteration of an `IO` loop deepens the C stack. That would undo the `do` tail-call fix measured at 20M iterations. Clang's `musttail` attribute could make those calls proper tail calls, but only between functions with the same signature, and `fk_apply`'s differs from that of the generated functions. Rejected. The same pattern, an IO runner with an explicit stack, is how Scala's cats-effect and ZIO run effects.

Cost: one allocation per `Bind` and per primitive action, plus the runner's dispatch. None of the 10 benchmarks performs effects in its loop, so they should not move. An effect-heavy program, for example one printing 1M lines, will be slower; this has not been measured. A specialization like the one for type classes could later run known primitive actions directly.

### REPL

As in GHCi, an input of type `IO a` is run, and its result printed unless it is `()`. Any other value is printed with `show`, as now.

### Implementation sketch

| Part | Change | Estimate |
|-|-|-|
| `check.c` | builtin type `IO`, arity 1, no constructors; `main` must have type `IO ()` | 30 lines |
| `prims.def`, `rt.c` | primitives return actions; `Pure`, `Prim` and `Bind` objects; `fk_run_io` | 120 lines |
| `parse.c` | `do` desugars to `$bindIO` and `$thenIO` | 20 lines |
| `lib/prelude.fk` | `return`, `>>=`, `>>`, `mapM_`, `mapM`, `when`, `trace`, `IO` versions of the I/O functions | 40 lines |
| `main.c`, `interp.c`, `cgen.c` | run `main`, run `IO` results in the REPL | 30 lines |

About 250 lines. These are estimates, not measurements.

### Migration

| 0.2 code | B1 |
|-|-|
| `line = getLine ()` | `line <- getLine` |
| `readInt (getLine ())` | `n <- fmap readInt getLine`, or `line <- getLine` then `let n = readInt line` |
| `early = print "x"`, run at startup | an action; run it from `main` |
| `print x` inside a pure function | a type error; use `trace` |
| `a >> b` on non-`IO` values | a type error |
| `f :: Int -> ()` that prints | `f :: Int -> IO ()` |

`fmap` for `IO` would be a plain function in B1 (`fmapIO`), and a `Functor` method in B2.

In this repo, 10 test and example files sequence effects. Most use `do` or `>>` on `print` and type-check unchanged. Three need edits: `io.fk` (`getLine ()`), `syntax.fk` (top-level `early` and `results` rely on running at startup) and `lazy.fk` (it tests effect order under lazy `let`, which becomes meaningless). A compile error for `getLine ()` should say "`getLine` is an action; write `line <- getLine`".

## A'' in detail

A `pure` annotation is a checked claim about one definition: calling the function with arguments free of effects performs no effects. The checker proves it, or rejects the program. The analysis exists: `fuse.c`'s purity fixpoint (`ok_e` without `tot`). The annotation adds syntax, rules for unknown calls and an error message.

### Syntax

A declaration listing names, placed like a fixity declaration:

```haskell
pure area, norm2, insert

area :: Shape -> Float
area (Circle r) = 3.14159 * r * r
```

Alternatives: a pragma, `{-# PURE area #-}`, would let Haskell tools ignore it, but funk has no pragma syntax. A marker in the signature, `area :: pure Shape -> Float`, reads as part of the type, which it is not.

### Meaning: pure modulo arguments

The claim is about the definition, not every call: the function may call its parameters, and their effects belong to its caller. This is what the inference computes, and it makes higher-order functions annotatable. `pure map` holds, though `map print xs` acts.

- **Allowed:** calls to pure globals, to parameters and to pattern variables bound from them, to local lambdas, and to `trace`. `trace` is exempt, so a debug print does not break an annotation, as with `Debug.Trace`.
- **Rejected:** a call to an effect primitive (`putStr`, `putStrLn`, `getLine`, `print`), to an impure global, or to a function value not traced to an argument.
- **Not total:** `error` and non-termination are allowed. Totality is fusion's other analysis.
- **Thunks:** the analysis walks lazy `let` and `where` bodies, so an effect in a thunk is caught.

### Unknown calls

The inference cannot see through three kinds of function values; the annotation needs a rule for each:

| Function value | Example | Rule |
|-|-|-|
| a function in data that came from a parameter | `applyAll fs x = map ($ x) fs` | allowed: it came from an argument, like a parameter |
| a class method through a dictionary | `showAll :: Show a => [a] -> [String]` | allowed: the dictionary is an argument. An instance may still print, e.g. `show` defined with `trace` |
| a closure from a global value, or returned by a call | `handlers = [print, ...]`, then `head handlers x` | rejected: the function does not come from the caller |

The first two keep "pure modulo arguments" consistent. The third is where the guarantee would leak, so it is rejected and the claim stays sound. The first needs an extension: today the analysis follows pattern variables bound from a parameter only when they are called directly, not elements of data passed through other functions.

### Errors

An error names the chain to the effect:

```text
shapes.fk:12:1: error: `report` is declared pure, but it can act:
  `report` calls `logLine` (shapes.fk:14:5), which calls `putStrLn` (shapes.fk:20:3)
```

The analysis returns one bit today; it would also record, per function, the first call that makes it impure.

### Limits

- **No new information for the optimizer.** The annotation checks the inference fusion already uses. Where the inference gives up, the annotation fails instead of adding facts.
- **No constraint on callers.** `map print xs` stays legal; the claim says nothing about arguments.
- **Opt-in.** Unannotated code keeps today's semantics. A'' is documentation the compiler verifies.

The next step would be annotating parameters, `applyTwice :: pure (a -> a) -> a -> a`, checked at each call site. The optimizer could then treat calls to that parameter as pure inside the function, which is where inference fails today. But that is a one-bit effect system in the types, with subtyping (a pure function goes where any function does) and questions about polymorphism. It is the start of option C, and no longer cheap.

### Cost

The `pure` declaration in the parser; the check after the purity fixpoint, which would then run even under `FK_NOFUSE`; provenance for errors; tests that accept `map`, `foldl` and a dictionary-using function and reject direct, indirect and global-closure effects. About 150 lines, an estimate.

## E in detail

E types effects in the function arrow, as Roc does, with effect polymorphism added: funk's functions are curried and higher-order, and Roc's separate `!` variants (`List.for_each!`) would split the prelude.

### Syntax

```haskell
print :: Show a => a ~> ()
getLine :: () ~> String
greet :: String ~> ()
greet name = putStrLn ("hello " ++ name)

map :: (a -> b) -> [a] -> [b]     -- pure if f is; acts if f does
area :: Shape -> Float             -- rejected if area can act
```

- **`a -> b` in a signature** means the function has no effects of its own. All the `->` arrows of one signature share an effect variable, so a higher-order function acts exactly when its function arguments do. That is A'''s "pure modulo arguments" ([A'' in detail](#a-in-detail)), stated in the type.
- **`a ~> b`** means the function may act.
- **`~>` over `=>`,** Roc's spelling, because `=>` separates a class context in funk and Haskell. `~>` is not a Haskell type operator in the standard. Other spellings would do as well; see question 6.

### Semantics

- Each arrow carries an effect: pure, effectful, or a variable. A function's effect is effectful if its body, when the function is applied, calls a function whose arrow is effectful.
- A pure function can be used where an effectful one is expected. Inference treats a pure function as polymorphic in its effect, so it unifies with either. This is the standard trick of effect systems, as in Koka, where a total function is polymorphic in its effect row.
- Evaluation is strict and left to right, as today, so effects happen in source order. There is no runner, and `>>` stays an ordinary primitive.
- **Lazy bindings.** A `let` or `where` binding whose evaluation can act is evaluated where it is bound, not at first use. Lazy bindings were the first problem in this note; this rule removes it. Pure bindings stay lazy, so `y = div 1 x` under a guard still does not fail.
- **Top-level values** must be pure: a top-level value with an effect is an error, except `main`, whose evaluation is the program. `act () = do ...`, a function, may act. Pure top-level values could then be evaluated at compile time, as in Roc.
- **Data fields** of function type are pure if written with `->`, effectful if written with `~>`. A field written `Int -> ()` cannot hold `print`.
- **Class methods** follow their signature. `show :: a -> String` is pure in every instance, so an instance cannot print, except through `trace`. A class that needs effects says so: `log :: a ~> ()`.
- **`trace` and `error`** are pure, as in Haskell.

### Inference

The arrow type gets a third argument, its effect: `Pure`, `Eff`, or a variable. Unification is unchanged. Each lambda gets a fresh effect variable. Every call in its body unifies the callee's arrow effect with that variable. A body with no effectful call leaves the variable free, so it generalizes, which is what makes a pure function usable at either effect.

Unifying all of a body's calls into one variable over-approximates: a function calling two function arguments makes their effects equal. That is sound, and at a call site it only costs precision when one argument is a monomorphic effectful function and the other a pure one.

### Partial application

Roc's functions take several arguments at once; funk's are curried. If every arrow of a signature shares the effect, applying an effectful function to fewer arguments than it takes also counts as acting, although it only builds a closure. `greeter = putStrLn . ("hi " ++)` would be rejected as a top-level value with an effect. Two refinements, to choose between with a prototype:

- **By arity.** A call is effectful only when it saturates a known function of known arity. The checker knows the arity of every global and every local lambda. Unknown function values keep the over-approximation.
- **Effect on the last arrow only.** `a -> b ~> c` would act only when given both arguments. That is more precise, but signatures have to say where the effect sits.

### Migration

| 0.2 code | E |
|-|-|
| `main = print x` | unchanged |
| `line = getLine ()` in `do` | unchanged; `getLine :: () ~> String` |
| `f :: Int -> ()` that prints | `f :: Int ~> ()` |
| `early = print "x"`, run at startup | an error; make it a function, or call it from `main` |
| `print x` inside a pure function | an error; use `trace`, or change the signature to `~>` |

Unannotated code needs no change, since effects are inferred. Of this repo's files, those that rely on top-level effects (`syntax.fk`) or on effects in lazy bindings (`lazy.fk`) change; the 15 benchmarks do not.

### Implementation sketch

| Part | Change | Estimate |
|-|-|-|
| `check.c` | an effect argument on `->`; effect variables per lambda; unification of call effects; the top-level, data-field and class-method rules; type printing that shows `~>` and hides free effect variables | 250 lines |
| `parse.c` | `~>` in types | 10 lines |
| `elab.c`, `resolve.c` | effectful bindings evaluated eagerly, through the existing eager `let` | 20 lines |
| `fuse.c` | purity from types, replacing the effect half of its analysis; totality stays | -100 lines, +30 |
| `prims.def`, prelude | effectful types for the I/O primitives | 20 lines |
| errors | "`f` is declared `Int -> ()` but calls `putStrLn` (line 4); write `Int ~> ()`" | 50 lines |

About 400 lines, minus the effect half of `fuse.c`'s analysis. These are estimates. The runtime does not change.

## Investigation (2026-10-25)

### Cost, measured

The new `printing` benchmark writes 1M lines with `putStrLn`, `show` and `++`, one action per line. Both runtime representations were simulated in today's funk and compiled:

| Version | 1M lines | Against untyped effects |
|-|-|-|
| C | 131 ms | |
| untyped effects (today) | 278 ms | |
| B1's runner: `Put`/`Seq` heap values, explicit continuation stack | 293 ms | +5% |
| `IO a = () -> a`, every `IO` function taking the token last | 284 ms | +2% |

Building each line costs far more than the action does. The runner's cost is real but small, even in a program that is nothing but effects; the other 15 benchmarks perform no effects in their loops.

The token representation was rejected above because unknown continuations deepen the C stack. Eta expansion, as GHC does with `State#`, plus funk's beta reduction makes most continuations known calls. One case remains: with `bindIO` as a library function, `ho_specialize` makes a copy of it, and a recursive loop then tail-calls back and forth between itself and the copy. That overflowed the stack at about 15M iterations. With bind inlined at compile time (`case m u of x -> body`), the same loop ran 20M iterations in constant stack. So the token representation needs `>>=` and `>>` compiled as special forms, plus an eta-expansion pass, for 3 points over the runner.

### Strict evaluation and `>>`

Haskell's `>>` is lazy in its second argument. funk's is not, and two programs show what that means for B1 if `>>` is an ordinary function, `a >> b = bindIO a (\_ -> b)`:

- `print "first" >> print (error "second")` raises `second` while the action is built, before `first` is printed. Haskell prints `first`, then fails.

- `forever a = a >> forever a` overflows the stack building the action, before anything runs. A loop written with `>>` builds every action before running the first.

Both were reproduced with the runner simulation. Hence the two corrections in "Semantics": `>>` is a special form, and `do` statements desugar through `bindIO` with a lambda. Library combinators that take an action as an argument, such as `when c act`, still build `act` eagerly. That is finite unless the argument is itself a recursive call, which is the case `>>` now covers.

### Effects and the optimizer

Under B1, `putStrLn s` builds an action and has no effect. Effects happen only in the runner. Consequences:

- **Fusion.** Its purity analysis treats the effect primitives as pure, and only `trace` stays impure. `mapM_ f (enumFromTo 1 n)` could then fuse if `mapM_` became a fusion consumer. That would remove the list `printing` builds today, whose marking is about 25% of its time at 20M lines.

- **Eager `let` and reuse.** No change: binding an action is a pure value.

### Syntax, proposed

| Question | Proposal | Reason |
|-|-|-|
| `getLine` | `getLine :: IO String` | Haskell's; `getLine ()` existed only because effects ran on evaluation |
| `>>` | special form | strict evaluation; see above |
| `>>=`, `return`, `pure` | plain functions; `pure` an alias of `return` | the continuation is already a lambda |
| `main` | must have type `IO ()`; an error otherwise, naming `main` | |
| `trace` | keep, as `Debug.Trace.trace` | debugging; the only effect outside `IO` |
| `when`, `unless`, `mapM_`, `forM_`, `sequence_` | in the prelude, over `IO` only | B2 generalizes them |

Every program in `bench/progs` reads its size with `readInt (getLine ())`, so all 15 change, to `main = do { line <- getLine; let n = readInt line; ... }`. The benchmark's measured part does not change.

### Recommendation

Of the monadic options, B1 with the runner, now. Its cost is 5% on a program that does nothing but print, and it needs no compiler work beyond the checker and `do`. E, added after this investigation, avoids that cost; see "The options compared". Token passing is a later optimization of the same semantics: 3 points, for an eta-expansion pass and inlined binds. If funk stays impure, A'' is the choice. It does not put effects in types, and it gives fusion nothing new.

These simulations stand in for the implementations; they share their allocation pattern, not their code. The prototypes are not in the repo.

## The options compared

### Expressiveness

| | A' | A'' | B1 (`IO` only) | B2 (`Monad` class) | E (effect arrows) |
|-|-|-|-|-|-|
| Effects visible | no: `f :: Int -> Int` may print | inferred, reported or checked against a `pure` annotation; unknown calls count as acting | in types | in types | in types, through closures, parameters and dictionaries |
| Higher-order functions with effects | any: `map print xs` works, ordered by strict evaluation | as A' | split: `map` and `mapM_`, `foldl` and `foldM` | split as B1, but `mapM` works for every monad | one function for both: `map print xs` works, and its type says it acts |
| Actions as values | by convention: `\() -> print x` | as A' | yes: stored, retried, combined | yes | as functions of `()`: `\() -> print x :: () ~> ()` |
| `do` for `Maybe`, `State`, parsers, lists | no | no | no | yes | no; independent of E, a `Monad` class could still come later |
| Printing from anywhere, for debugging | yes | yes | `trace` | `trace` | `trace` |
| Remaining surprises | lazy `let` and `where` outside `do` still move effects; a top-level `act = ...` runs at startup; `x <- 1 + 2` is accepted | as A'; a `pure` annotation can catch some | none of these | none of these | `x <- 1 + 2` is accepted, harmlessly; partial application can over-approximate effects |
| Haskell compatibility | Haskell syntax, OCaml meaning | as A' | Haskell's `IO` code ports | most Haskell code ports | pure code ports unchanged; `IO` code does not |

A', A'' and E keep composition without coloring, which is the ML experience and an advantage for small programs; E also puts it in the types. B2 adds `do` for every monad, with three costs specific to a strict language:

- **Recursive applicative definitions loop.** `some p = (:) <$> p <*> many p` evaluates `many p` while building the parser and never terminates. `do` style is safe, because the recursive call is under a lambda. OCaml parser-combinator libraries need explicit thunks for the same reason.
- **`>>` must be lazy for every monad,** not only `IO`, so the special form applies to all of them.
- **Kinds.** `m :: * -> *` and phase 2 of type classes ([type-classes.md](type-classes.md#later-phases)) are a second type-class project. B1 needs neither.

### Performance

| | A' | A'' | B1 | B2 | E |
|-|-|-|-|-|-|
| Effect-heavy code (`printing`) | 0% | 0% | +5% with the runner, +2% with token passing | as B1 for `IO` | 0%: calls stay direct |
| Pure code (14 of 15 benchmarks) | 0% | 0% | 0% | 0% for `IO` | 0% |
| Code generic over `Monad m` | n/a | n/a | n/a | dictionary calls, unless specialized at a known monad: the clone machinery handles kind `*` and would need extending to `* -> *` | n/a |
| User monads (`State`, parsers) | n/a | n/a | n/a | a closure and an unknown call per `>>=`. funk has no inliner and depends on specialization and beta reduction to remove them; GHC needs heavy inlining for the same. Expect such code to be slow at first | n/a |
| What the optimizer knows | purity inferred, giving up at unknown calls | as A' | purity from types: a function not returning `IO` is pure, except `trace` and `error` | as B1 | purity from types, per arrow: a call is pure unless its arrow is effectful at that instance |

The last row is B1's and E's performance argument, and for B1 it partly offsets the 5%. Fusion today refuses pipelines whose stage functions are closures, parameters or dictionary methods, because it cannot rule out effects. Under B1 and E their types rule them out. E keeps today's direct calls, so it gets this without B1's cost. More pipelines would fuse, more `let`s could be evaluated eagerly, and `mapM_` over a range could fuse, removing the list behind about 25% of `printing`'s time at 20M lines. This gain is plausible, not measured.

### Weighing

- **A'** fits if funk stays an ML with Haskell syntax. It costs nothing, but keeps the mismatch this note began with: a Haskell reader assumes a purity the types do not promise.
- **A''** narrows that mismatch for little work: the analysis exists, and only a report or an annotation check is new. Its guarantee stops at unknown calls, which are common in higher-order code.
- **B1** gets most of the value: types that tell the truth, `do` that means what it says, and an optimizer that knows purity. It costs about 250 lines, 5% on effect-heavy code, and a migration of every program's first line.
- **B2** adds generality whose costs are specific to a strict language. It is better decided on its own, once a program needs a monad other than `IO`, than committed to now as "B1 then B2".
- **E** has B1's honest types and optimizer knowledge at A'''s cost, 0%. It keeps composition without coloring and needs little migration, because funk's runtime semantics do not change. It costs Haskell compatibility for `IO` code, about 400 lines in the checker, and two unproven points: whether unification-based effect inference is precise enough on real code, and how partial application is treated. Roc shows the approach in a strict language, but without currying or effect polymorphism.

Recommendation: E, if a prototype of its inference, run over this repo's programs, rejects no correct program that a refinement cannot fix. Otherwise B1, with B2 decided later. A'' if funk stays impure; it subsumes A'.

## Questions for the decision

1. **A', A'', B1 or E? And B2 now, later or never?** B1 gives every benefit except `do` for other monads, for 5% on effect-heavy code. E gives B1's types at no runtime cost, but not Haskell's `IO`. Waiting for B2 means kinds and phase 2 of type classes come first, a larger change; see "The options compared".

2. **`getLine` or `getLine ()`?** Haskell compatibility says `getLine :: IO String`. Keeping `()` would only ease migration.

3. **`>>` as a special form?** Required for B1 in a strict language, and it makes `>>` the one operator users cannot redefine. The alternative, a lazy `>>` through an implicit thunk, adds a laziness rule for one operator.

4. **Keep `trace`?** Without it, printing from pure code is impossible, which is the point of B1 but hurts debugging.

5. **Accept the break?** funk is pre-1.0, and 0.2 already breaks `deriving` and `f.g`. Releasing 0.2 first and then B1 or E as 0.3 gives users one version with `do` over untyped effects.
6. **Under E, the spelling of the effectful arrow:** `~>`, or another token such as `-!>`? And partial application: by arity, or the effect on the last arrow only?

The citations are from memory and were not checked in this session.
