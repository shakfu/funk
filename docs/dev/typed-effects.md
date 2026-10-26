# Typed effects: design note

Status: decided and implemented in 0.3.0: option E, effect arrows, spelled `a ->> b`; see "Implementation" in "E in detail". This note records the options, the measurements and the prototype that led there. Earlier status lines recommended A', for performance, then B1; "Investigation" measured B1's cost at +5%, and E avoids it. The current design is summarized in [design.md](design.md#effects-effect-arrows).

## Problem

Effects are untyped. `print` has type `Show a => a -> ()` and prints when the call is evaluated. Three later features made the consequences visible:

- **Lazy `let` moves effects.** `let a = getLine ()` reads at the first use of `a`, not at its line. `let a = print 1 in a >> a` prints once, since the binding is memoized.

- **`do` cannot mean what it says.** `<-` accepts any expression, `x <- 1 + 2` included. A top-level `act = do ...` is a value, so it runs once, at startup. A test in this repo fell into this while `do` was being written.

- **Types hide effects.** `f :: Int -> Int` may print, read input or exit. A Haskell reader assumes it does not.

None of these is a bug in the implementation. They come from Haskell syntax over OCaml semantics, with Haskell's lazy `let` added.

## Why `let` is lazy

funk is strict except for non-recursive `let` and `where` value bindings. The rationale in [design.md](design.md) argues for `where` only: a `where` binding is written after the guards it scopes over, and a strict one would run before them, so `f x | x == 0 = 0 | otherwise = y where y = div 1 x` would fail on `f 0`. `let` was made lazy with it, in 0.2, and its case is weaker.

For a lazy `let`:

- **Haskell compatibility.** `f x = let y = div 1 x in if x == 0 then 0 else y` is valid Haskell, and fails on `f 0` with a strict `let`. It is rarer than the `where` form, and occurs in `do` blocks too. The difference would show at run time, not as a compile error.
- **Uniformity.** Haskell defines `where` as a `let` scoped over the guards. One rule for both is simpler, and funk implements both with the same thunks.

For a strict `let`:

- **Source order.** A `let` precedes its use, so strict evaluation reads naturally, as in ML. The guard problem exists only because `where` follows the code it scopes over.
- **Effects.** A lazy `let` moves effects: `let a = getLine ()` reads at the first use of `a`. That is the first problem above, and why A' makes `let` statements in `do` strict.

Speed does not decide it. Eager `let` already evaluates a binding where it is bound when its right side cannot fail, loop or act, so both rules generate the same code for most bindings ([performance-first.md](performance-first.md)). The choice is about meaning only, and it depends on the options below:

- **Under E,** an effectful binding must be a `let` statement in `do`, which runs on its line; in `where` or `let ... in` it is an error. That removes the harm. What laziness keeps is that an unused binding cannot fail or loop, and there lazy is the Haskell-compatible choice. `let` stays lazy.
- **Under A or A',** the question stands. Strict `let` statements in `do`, as A' proposes, with expression `let` left lazy for compatibility, give `let` two rules, which is A''s price.
- **Under B1,** binding an action does not run it, so a lazy `let` is harmless for effects, as for E.

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
| E | Effect arrows (Roc): `a -> b` pure, `a ->> b` effectful; calls stay direct | yes, one bit per arrow | medium: an effect on every arrow | rarely: effectful signatures change |

- **A** is what strict ML-family languages do, and it works there because their `let` is strict. Effect order is then the order of the source.

- **A'** removes the `do` surprise: a `let` statement in `do` would run at its line, as `<-` does. Effects stay invisible in types, top-level effects still run at startup, and `let` would mean two things.
- **A''** is A' plus the purity that `fuse.c` already infers ([fusion.md](fusion.md#semantics-when-fusion-is-exact)), made visible: a `pure` declaration the checker verifies, rejecting the program if the function can act ("A'' in detail"). Effects compose with any function, as in A', and readers and the optimizer learn some of what B1 would tell them. The guarantee is weaker than a type: inference stops at unknown calls (closures, parameters, dictionaries) and must assume they act.

- **B1** is Haskell's `IO` ([Peyton Jones and Wadler, POPL 1993](https://dl.acm.org/doi/10.1145/158511.158524)), with `bindIO :: IO a -> (a -> IO b) -> IO b` as a plain function. `do` works for `IO` only. No higher-kinded classes are needed.

- **B2** is B1 with `class Monad m` over `m`, which needs kinds. `do` then also works for `Maybe`, lists, parsers and state.

- **C** types each effect separately: `print : a -> <console> ()` ([Leijen, MSFP 2014](https://arxiv.org/abs/1406.2061)). It is the most precise, but it needs row polymorphism in the checker, about the size of type classes again. Its syntax is not Haskell's.

- **D** threads a unique `World` value: `getLine :: World -> (String, World)`. Code becomes verbose without new syntax. Rejected.

- **E** puts one bit in every function arrow: pure or effectful. There is no `IO` type and no runner; an effectful call is a direct call, as today. Roc does this with `->` and `=>` ([roc-lang.md](roc-lang.md#1-effects-in-the-arrow)). funk spells the effectful arrow `->>`, because `=>` already marks class contexts. A higher-order function is polymorphic in its argument's effect, so one `map` serves both (see "E in detail").

Recommendation: E, whose inference has been prototyped; see "Prototype" and "The options compared". Otherwise B1, with B2 decided later, on its own. B1 is source-compatible with B2, because a `do` over `IO` written for B1 means the same under `Monad IO`. If funk stays impure, choose A'': it includes A' and fixes the `do` surprise.

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
print :: Show a => a ->> ()
getLine :: () ->> String
greet :: String ->> ()
greet name = putStrLn ("hello " ++ name)

map :: (a -> b) -> [a] -> [b]     -- pure if f is; acts if f does
area :: Shape -> Float             -- rejected if area can act
```

- **`a -> b` in a signature** means the function has no effects of its own. All the `->` arrows of one signature share an effect variable, so a higher-order function acts exactly when its function arguments do. That is A'''s "pure modulo arguments" ([A'' in detail](#a-in-detail)), stated in the type.
- **`a ->> b`** means the function may act.
- **`->>`, decided over three alternatives** (2026-10-26):
  - `=>`, Roc's spelling, already separates a class context in funk and Haskell. One signature could hold both meanings, as in `print :: Show a => a => ()`.
  - `~>`, the first proposal, differs from `->` by one stroke, which is easy to miss in many fonts.
  - `-!>` stands out as much, but breaks the arrow.

  `->>` stays an arrow, is longer than `->` in aligned signatures, and borrows `>>`, which Haskell readers know as effect sequencing. No funk source contains `->>`, and the lexer already reads it as one token.

### What changes in programs

E is mostly a checker change. Its only new syntax is `->>` in types; expressions do not change.

```haskell
-- 0.2                                   -- E
greet :: String -> ()                    greet :: String ->> ()
greet n = putStrLn ("hi " ++ n)          greet n = putStrLn ("hi " ++ n)

data Handler = Handler (Int -> ())       data Handler = Handler (Int ->> ())  -- holds an effectful function
class Logger a where log :: a -> ()      class Logger a where log :: a ->> ()  -- instances may act
```

- **`->>` appears** wherever a type is written: signatures, data fields, class methods. Unannotated code needs nothing, since effects are inferred.
- **Unchanged:** calls (`print x`, `getLine ()`), `do`, `<-`, `let` statements and `>>`. There is no `!` suffix on effectful names: Roc's is a naming convention, and `!` cannot be part of a funk identifier. There is no `IO` type, no `return`, no `>>=`, and no new keyword.

What changes is which programs are accepted:

| Rule | Effect on programs |
|-|-|
| A signature with `->` is checked | `loud :: Int -> ()` that prints is an error; write `->>` |
| Top-level values are pure, except `main` | `early = print 1` is an error; make it a function, `early () = ...`, or run it from `main` |
| An effectful binding is a `let` statement in `do` | it runs on its line; in `where` or `let ... in`, it is an error |
| Pure fields and methods stay pure | `Handler print` is an error if the field is `Int -> ()` |

Against B1's changes:

| | B1 | E |
|-|-|-|
| Effectful signatures | `String -> IO ()` | `String ->> ()` |
| Reading input | `line <- getLine`, without `()` | `getLine ()`, unchanged |
| `main` | `main :: IO ()`, built from actions | unchanged |
| `>>` | a special form, lazy in its second operand | unchanged |
| New prelude names | `return`, `>>=`, `mapM_`, `when` and others, over `IO` | none: `mapM_` stays a fold |
| Printing from pure code | `trace` | `trace` |

Pure Haskell code parses and checks unchanged, because its `->` signatures are honest. Haskell code typed with `IO` does not work: funk has no `IO` type. B1 would make that code port, though `IO` is one of about fifteen gaps between Haskell and funk source ([haskell-compat.md](haskell-compat.md)).

Two things E's syntax, as designed, cannot say:

- **A pure argument, regardless.** `(a -> b) -> c` means the function acts when its argument does. Requiring the argument itself to be pure needs another marker. Data fields have that rigidity; signatures do not.
- **Independent effects for two arguments.** All arrows of one signature share one variable, so a function taking two callbacks acts if either does. That is sound, and no program in the prototype's corpus needed more.

Both would need an explicit effect variable in the syntax, as Koka writes it. Neither came up in the prototype.

### Semantics

- Each arrow carries an effect: pure, effectful, or a variable. A function's effect is effectful if its body, when the function is applied, calls a function whose arrow is effectful.
- A pure function can be used where an effectful one is expected. Inference treats a pure function as polymorphic in its effect, so it unifies with either. This is the standard trick of effect systems, as in Koka, where a total function is polymorphic in its effect row.
- Evaluation is strict and left to right, as today, so effects happen in source order. There is no runner, and `>>` stays an ordinary primitive.
- **Lazy bindings.** A binding whose evaluation can act must be a `let` statement in `do`, and runs on its line. In `where` or `let ... in` it is an error, with a hint: bind it with `<-` or a `let` statement, or make it a function. Lazy bindings were the first problem in this note; this rule removes it without moving any effect silently. Pure bindings stay lazy, so `y = div 1 x` under a guard still does not fail. The design first evaluated every effectful binding where it is bound; see "Implementation" for why that was wrong for `where`.
- **Top-level values** must be pure: a top-level value with an effect is an error, except `main`, whose evaluation is the program. `act () = do ...`, a function, may act. Pure top-level values could then be evaluated at compile time, as in Roc.
- **Data fields** of function type are pure if written with `->`, effectful if written with `->>`. A field written `Int -> ()` cannot hold `print`.
- **Class methods** follow their signature. `show :: a -> String` is pure in every instance, so an instance cannot print, except through `trace`. A class that needs effects says so: `log :: a ->> ()`.
- **`trace` and `error`** are pure, as in Haskell.

### Inference

The arrow type gets a third argument, its effect: `Pure`, `Eff`, or a variable. Unification is unchanged. Each lambda gets a fresh effect variable. Every call in its body unifies the callee's arrow effect with that variable. A body with no effectful call leaves the variable free, so it generalizes, which is what makes a pure function usable at either effect.

Unifying all of a body's calls into one variable over-approximates: a function calling two function arguments makes their effects equal. That is sound, and at a call site it only costs precision when one argument is a monomorphic effectful function and the other a pure one.

### Partial application

Roc's functions take several arguments at once; funk's are curried. If every arrow of a signature shares the effect, applying an effectful function to fewer arguments than it takes also counts as acting, although it only builds a closure. `greeter = putStrLn . ("hi " ++)` would be rejected as a top-level value with an effect. Two refinements, to choose between with a prototype:

- **By arity.** A call is effectful only when it saturates a known function of known arity. The checker knows the arity of every global and every local lambda. Unknown function values keep the over-approximation.
- **Effect on the last arrow only.** `a -> b ->> c` would act only when given both arguments. That is more precise, but signatures have to say where the effect sits.

The prototype chose by arity; see "Prototype". A lambda's inferred type already puts the effect on the arrow that runs its body (`mk :: a -> a ->> ()`), and a signature may write `->>` on its last arrow only.

### Migration

| 0.2 code | E |
|-|-|
| `main = print x` | unchanged |
| `line = getLine ()` in `do` | unchanged; `getLine :: () ->> String` |
| `f :: Int -> ()` that prints | `f :: Int ->> ()` |
| `early = print "x"`, run at startup | an error; make it a function, or call it from `main` |
| `print x` inside a pure function | an error; use `trace`, or change the signature to `->>` |

Unannotated code needs no change, since effects are inferred. Of this repo's files, those that rely on top-level effects (`syntax.fk`) or on effects in lazy bindings (`lazy.fk`) change; the 15 benchmarks do not.

### Implementation sketch

| Part | Change | Estimate |
|-|-|-|
| `check.c` | an effect argument on `->`; effect variables per lambda; unification of call effects; the top-level, data-field and class-method rules; type printing that shows `->>` and hides free effect variables | 250 lines |
| `parse.c` | `->>` in types | 10 lines |
| `elab.c`, `resolve.c` | effectful bindings evaluated eagerly, through the existing eager `let` | 20 lines |
| `fuse.c` | purity from types, replacing the effect half of its analysis; totality stays | -100 lines, +30 |
| `prims.def`, prelude | effectful types for the I/O primitives | 20 lines |
| errors | "`f` is declared `Int -> ()` but calls `putStrLn` (line 4); write `Int ->> ()`" | 50 lines |

About 400 lines, minus the effect half of `fuse.c`'s analysis. These are estimates. The runtime does not change.

### Prototype (2026-10-26)

The inference is prototyped in `check.c`, enabled by `FK_EFFECTS`; without it nothing changes. It reports what E would reject instead of failing, and prints each top-level type with its effects. Not prototyped: evaluating effectful lazy bindings where they are bound, and replacing `fuse.c`'s analysis. Three modes:

| Mode | Rule |
|-|-|
| 1 | every applied arrow counts as a call that may act |
| 2 | only the arrow that saturates a function of known arity: globals, primitives, class methods, lambdas applied directly |
| 3 | 2, and a call of a function whose type is not yet known, such as a record field resolved after its binding group, is settled at the end of the group instead of at the call |

Results:

| | Mode 1 | Mode 2 | Mode 3 |
|-|-|-|-|
| The repo's 39 programs and the prelude | 6 rejections | 6 rejections | 4 rejections |
| `tests/effects/reject.fk`: 15 channels through which an effect reaches pure code | 15 of 15 rejected | 15 of 15 | 15 of 15 |
| `tests/effects/accept.fk`: 13 bindings that must be accepted | 1 rejected: `greeter = putStrLn . ("hi " ++)` | 0 | 0 |
| Time to check `examples/classes.fk` | | | 6.3 ms, against 6.2 ms without effects |

In mode 3 the four rejections in the repo are the ones E intends: top-level values that act at startup, `early`, `main'` and `results` in `syntax.fk` and `printing` in `fuse.fk`. Modes 1 and 2 add two false rejections in `records.fk`. There, `((counter 0).incr ()).get ()` calls a function from a record field before the field's type is resolved. The call took the effect of `main`, and then met the field's pure arrow. Mode 1 also rejects `greeter`: it counts the partial application `(++) "hi "` as a call.

The 15 channels in `reject.fk`: a function argument, a local function, a data field, a list element, a function value, a local polymorphic function, an argument of a signature's shared effect, a signature that says pure over a body that prints, `>>`, a record field, mutual recursion, `map` in a value, a class-constrained signature, an instance of a pure method, and a returned function. All three modes reject all 15.

The prototype also reports 15 lazy bindings that act and that E would evaluate where they are bound. Seven are the benchmarks' `let n = readInt (getLine ())`, used at once, so their order does not change. `lazy.fk` and `do.fk` test effect order under lazy bindings and would change, as "Migration" says.

The design changed in three places to get there:

- **Class methods take a signature's shared effect variable, not rigid `Pure`.** With `Pure`, `putStrLn . (++) "hi "` was rejected: composing a pure method with an effectful function merged their effects. A signature's variable is polymorphic at uses and becomes rigid when an instance is checked against it, so an instance that prints is still rejected.
- **Arity is the arity before elaboration.** A constrained function gains a dictionary parameter when elaborated, so `print` looked like arity 2, and `print x` did not count as a call. A class method's arity is its signature's, not its selector's.
- **Substituting a class's type variable** into a method's scheme for an instance must keep the effects, or the instance escapes the check.

Mode 3 is the design: E as described, with the arity rule and deferred calls. The repo's programs are small, so they do not show whether merging all of a body's calls into one effect is precise enough on larger code. Neither do they show the error messages' quality.

### Implementation (2026-10-26)

Mode 3 of the prototype is now the checker, always on; `FK_EFFECTS` and modes 1 and 2 are gone. Effect errors are compile errors. Where the implementation goes beyond the design:

- **Effectful bindings are `do` statements; elsewhere they are errors.** The design evaluated every effectful `let` and `where` binding where it is bound. That breaks a common idiom: in

  ```haskell
  countdown n
    | n == 0 = putStrLn "liftoff"
    | otherwise = rest
    where rest = print n >> countdown (n - 1)
  ```

  `rest` ran before the guards that protect it, so `countdown` never stopped. In Haskell this works because `rest` is an `IO` action, a value that runs only when sequenced; under E, evaluating `rest` is running it. Now such a binding is an error, with the hint "bind it in `do` (`rest <- ...`, or a `let` statement), or make it a function (`rest () = ...`)". A `let` statement in `do` still runs on its line. No effect moves silently, under either rule.
- **Which bindings can act.** A local value binding can act if its own effect is `Eff`, or the effect of an argument of any function around it, top-level or local, which callers choose. It is decided at the end of the binding's group, before its effect is merged into the enclosing function's. Merging first made a pure binding inside a function that prints look effectful, and `where y = div 1 x` under a guard failed. A free effect means only pure calls, and is not merged. An effect tied to something outside the binding, such as a recursive call of the function being inferred, is decided after that function is generalized: a recursive call of a pure function (`where y = depth (n - 1) + 1`) stays lazy.
- **A function that acts and calls a function argument** writes that argument with `->>`: `withArg :: (Int ->> ()) ->> ()`. A body has one effect, and calling the argument merges the argument's effect into it, so a `->` argument would have to be `Eff`. Callers lose little: a lambda or a pure global passed as the argument is polymorphic in its effect.
- **Error messages** name the binding, and for an instance method the instance: "in `shout` of `instance Shout Bool`: declared with `->` but calls an effectful function; its signature needs `->>`".
- **`trace :: String -> a -> a`** is a primitive. It flushes standard output, writes its message to standard error, and is pure in its type.
- **Not done:** `fuse.c` keeps its own purity analysis; elaboration's type copies drop arrow effects, so it cannot read them yet ([TODO.md](../../TODO.md)).

Tests: 18 error tests (`tests/errors/effect_*.fk`), one per effect channel plus effectful `where`, `let ... in`, and `let ... in` over an argument's effect; and `tests/run/effects.fk`, which covers accepted programs, the order of `let` statements, the lazy recursive call, an argument's effect and `trace`. `syntax.fk` and `fuse.fk` no longer run effects at startup. `io.fk` reads with `<-`, `do.fk`'s `where` helper is a function, and `lazy.fk` shows at-most-once evaluation with `trace`. The benchmarks' `main = let n = readInt (getLine ()) in ...` became `main = do` with a `let` statement; their C versions and results do not change.

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

## One program in each style

The same program in each style. It reads a count and then `name score` lines, grades each student with a pure function, reports each with an effectful one, and prints the average. With this input:

```text
3
ada 95
bob 80
cyd 60
```

it prints `ada: A`, `bob: B`, `cyd: C` and `average: 78`. A and E were run: A as it stands in 0.2, in both backends, and E's version under the prototype checker (`FK_EFFECTS=3`), which accepts it. B1 and B2 use syntax funk does not have; they were not run.

### A: untyped effects, as in 0.2

```haskell
data Student = Student { name :: String, score :: Int }

grade :: Int -> String
grade s = if s >= 90 then "A" else if s >= 75 then "B" else "C"

parse :: String -> Student
parse line = case words line of
  [n, s] -> Student n (readInt s)
  _ -> error ("bad line: " ++ line)

average :: [Student] -> Int
average ss = div (sum (map (\s -> s.score) ss)) (length ss)

readStudents :: Int -> [Student]
readStudents k =
  if k == 0 then []
  else let s = parse (getLine ()) in s : readStudents (k - 1)

report :: Student -> ()
report s = putStrLn (s.name ++ ": " ++ grade s.score)

main = do
  n <- readInt (getLine ())
  students <- readStudents n
  mapM_ report students
  putStrLn ("average: " ++ show (average students))
```

`readStudents` and `report` claim `->`, but read input and print; nothing checks them. Order depends on two rules a reader has to know: `s` is a lazy binding, forced when the cons cell is built, which is before `readStudents (k - 1)` runs; and `<-` evaluates its expression at its line.

### A': strict `let` statements in `do`

The text is A's. A' changes only `let` statements in `do`, and this program has none, so it behaves as A. `let s = ...` in `readStudents` is an ordinary `let`, and stays lazy.

### A'': A' with `pure` declarations

A's text, plus one line:

```haskell
pure grade, parse, average
```

The checker verifies the three. `readStudents` and `report` still claim `->` unchecked: A'' checks only what is declared `pure`.

### B1: `IO`

```haskell
data Student = Student { name :: String, score :: Int }

grade :: Int -> String
grade s = if s >= 90 then "A" else if s >= 75 then "B" else "C"

parse :: String -> Student
parse line = case words line of
  [n, s] -> Student n (readInt s)
  _ -> error ("bad line: " ++ line)

average :: [Student] -> Int
average ss = div (sum (map (\s -> s.score) ss)) (length ss)

readStudents :: Int -> IO [Student]
readStudents k =
  if k == 0 then return []
  else do
    line <- getLine
    rest <- readStudents (k - 1)
    return (parse line : rest)

report :: Student -> IO ()
report s = putStrLn (s.name ++ ": " ++ grade s.score)

main :: IO ()
main = do
  line <- getLine
  let n = readInt line
  students <- readStudents n
  mapM_ report students
  putStrLn ("average: " ++ show (average students))
```

The pure part is unchanged. Every effectful line changes: `getLine` loses its `()`, results go through `return`, `main` builds an action, and the input loop becomes a `do` block. `parse line : rest` is built after all lines are read, so a bad line fails only then.

### B2: a `Monad` class

B2 runs B1's program unchanged. Its gain shows when another monad is used; here `Maybe` replaces `error` for bad input:

```haskell
data Student = Student { name :: String, score :: Int }

grade :: Int -> String
grade s = if s >= 90 then "A" else if s >= 75 then "B" else "C"

parseScore :: String -> Maybe Int
parseScore s = if s /= "" && all isDigit (unpack s) then Just (readInt s) else Nothing

parse :: String -> Maybe Student
parse line = case words line of
  [n, s] -> do
    v <- parseScore s
    return (Student n v)
  _ -> Nothing

average :: [Student] -> Int
average ss = div (sum (map (\s -> s.score) ss)) (length ss)

report :: Student -> IO ()
report s = putStrLn (s.name ++ ": " ++ grade s.score)

main :: IO ()
main = do
  line <- getLine
  ls <- replicateM (readInt line) getLine
  case mapM parse ls of
    Nothing -> putStrLn "bad input"
    Just students -> do
      mapM_ report students
      putStrLn ("average: " ++ show (average students))
```

`do` in `parse` is over `Maybe`, `mapM parse ls` is over `Maybe` too, and `replicateM` is over `IO`. Each needs a function generic over `Monad m`.

### E: effect arrows

```haskell
data Student = Student { name :: String, score :: Int }

grade :: Int -> String
grade s = if s >= 90 then "A" else if s >= 75 then "B" else "C"

parse :: String -> Student
parse line = case words line of
  [n, s] -> Student n (readInt s)
  _ -> error ("bad line: " ++ line)

average :: [Student] -> Int
average ss = div (sum (map (\s -> s.score) ss)) (length ss)

readStudents :: Int ->> [Student]
readStudents k =
  if k == 0 then []
  else do
    s <- parse (getLine ())
    s : readStudents (k - 1)

report :: Student ->> ()
report s = putStrLn (s.name ++ ": " ++ grade s.score)

main = do
  n <- readInt (getLine ())
  students <- readStudents n
  mapM_ report students
  putStrLn ("average: " ++ show (average students))
```

Three things differ from A: the two signatures say `->>`,

```text
readStudents :: Int ->> [Student]
report :: Student ->> ()
```

and the input loop's `let s = parse (getLine ()) in ...` becomes a `do` block. E rejects the `let ... in` form, because `s` reads input and would run at its first use; in `do`, `s <- ...` reads on its line. The checker infers everything else.

### Summary

| Style | Lines changed from A | Checked |
|-|-|-|
| A | | nothing |
| A' | none, for this program | nothing |
| A'' | 1 added | `grade`, `parse`, `average` |
| B1 | the input loop, `report`'s and `main`'s types, `main`'s first line | every function's effects |
| B2 | as B1; more where another monad is used | as B1, for every monad |
| E | 3: the two effectful signatures, and the input loop's `let ... in` as a `do` | every function's effects |

## The options compared

### Expressiveness

| | A' | A'' | B1 (`IO` only) | B2 (`Monad` class) | E (effect arrows) |
|-|-|-|-|-|-|
| Effects visible | no: `f :: Int -> Int` may print | inferred, reported or checked against a `pure` annotation; unknown calls count as acting | in types | in types | in types, through closures, parameters and dictionaries |
| Higher-order functions with effects | any: `map print xs` works, ordered by strict evaluation | as A' | split: `map` and `mapM_`, `foldl` and `foldM` | split as B1, but `mapM` works for every monad | one function for both: `map print xs` works, and its type says it acts |
| Actions as values | by convention: `\() -> print x` | as A' | yes: stored, retried, combined | yes | as functions of `()`: `\() -> print x :: () ->> ()` |
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

Recommendation: E. The prototype of its inference, with two refinements, rejects only what E intends in this repo's programs, and catches all 15 effect channels in its test ("Prototype"). Otherwise B1, with B2 decided later. A'' if funk stays impure; it subsumes A'.

## Questions for the decision

1. **A', A'', B1 or E? And B2 now, later or never?** B1 gives every benefit except `do` for other monads, for 5% on effect-heavy code. E gives B1's types at no runtime cost, but not Haskell's `IO`. Waiting for B2 means kinds and phase 2 of type classes come first, a larger change; see "The options compared".

2. **`getLine` or `getLine ()`?** Haskell compatibility says `getLine :: IO String`. Keeping `()` would only ease migration.

3. **`>>` as a special form?** Required for B1 in a strict language, and it makes `>>` the one operator users cannot redefine. The alternative, a lazy `>>` through an implicit thunk, adds a laziness rule for one operator.

4. **Keep `trace`?** Without it, printing from pure code is impossible, which is the point of B1 but hurts debugging.

5. **Accept the break?** funk is pre-1.0, and 0.2 already breaks `deriving` and `f.g`. Releasing 0.2 first and then B1 or E as 0.3 gives users one version with `do` over untyped effects.
6. ~~**Under E, the spelling of the effectful arrow.**~~ Decided: `->>` ("Syntax" in "E in detail"). Partial application is settled by arity, as the prototype showed.

The citations are from memory and were not checked in this session.
