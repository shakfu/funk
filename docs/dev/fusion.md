# List fusion: design note

Status: implemented (unreleased), with exact semantics and benchmark option (c); see "Result". It follows [reuse.md](reuse.md), which writes `map`'s output into its input's cells; fusion removes the intermediate list instead.

## What fusion would gain

The pipelines of `lists` and `queens` were fused by hand in funk source and compiled, on an M1:

| Program | Today | Fused by hand | C |
|-|-|-|-|
| `lists`: `sum (map (* 3) (filter even (enumFromTo 1 n)))` | 55 ms (2.6x) | 4-6 ms | 22 ms with lists; 3-5 ms as a loop |
| `queens`: `concatMap (\qs -> map (: qs) (filter (\q -> safe q qs) (enumFromTo 1 n)))` | 45 ms (2.9x) | 25 ms (1.6x) | 16 ms |

`queens.c` already runs the fused loops: it builds only the output boards. `lists.c` builds every intermediate list on purpose, so a fused `lists` would run 4x faster than its C version. See decision 2.

## Approaches

| | Approach | Needs | Note |
|-|-|-|-|
| A | `foldr`/`build` with rewrite rules (GHC, [Gill, Launchbury and Peyton Jones, FPCA 1993](https://dl.acm.org/doi/10.1145/165180.165214)) | a general inliner and rules; `foldl` through `foldr` needs arity analysis to become a loop | large |
| B | Stream fusion as a library (`Stream` with `Yield`/`Skip`/`Done`) ([Coutts, Leshchinskiy and Stewart, ICFP 2007](https://dl.acm.org/doi/10.1145/1291151.1291199)) | case-of-case and constructor specialization, or every step allocates a `Step` | large |
| C | Pipeline fusion over a fixed set of prelude functions, at compile time (chosen) | a recognizer and a loop generator | small; covers only the listed functions |
| D | Opt-in: a fusing library that programs call explicitly | nothing in the compiler | no semantic risk, but no existing program speeds up |

C is B done at compile time: the stages are known, so the loop is generated directly and no `Step` value exists. It reuses existing passes for the rest: the loop is a local recursive function, so `lift_locals` lifts it, `ho_specialize` makes its stage functions direct calls, the typed backend gives it scalar locals, and TRMC builds its output list.

## Semantics: when fusion is exact

Evaluation is strict, so `sum (map f (filter p xs))` runs `p` on every element, then `f` on every kept element, then sums. Fused, the calls interleave: `p x1`, `f x1`, `p x2`. Three things could become observable:

- **Effects.** `print` acts when evaluated ([design.md](design.md#effects-impure-primitives-sequenced-by-)), so interleaving reorders output.
- **Which error.** If `p` fails on element 5 and `f` on element 1, the original raises `p`'s error and the fused loop raises `f`'s.
- **Whether it fails.** `take 2 (map f [1, 2, 0])` evaluates `f 0` in the original. Fused, it stops after two elements and does not.

A pipeline is fused only if:

1. no stage can perform an effect;
2. at most one stage can fail, by raising an error or by not terminating;
3. every stage before a short-circuiting stage (`take`, `takeWhile`, `any`, `all`, `elem`) cannot fail.

Under these conditions the fused program has the same value, error and termination as the original. Only time and memory differ. Proof sketch: by 1, only the value and failure are observable. By 2, the elements are processed in the same order, and the one stage that can fail sees the same inputs up to its first failure. By 3, the elements a short-circuit skips would have run only total, pure code.

Neither condition needs an annotation. Two analyses, both greatest fixpoints over the chunk's functions like those in [reuse.md](reuse.md):

- **Pure:** the expression calls no effectful primitive (`putStr`, `getLine`, and the prelude functions built on them) and no unknown function, such as a parameter or a closure in a local variable.
- **Total:** pure, and every call is to a total primitive or a non-recursive total function. Arithmetic is total, except `div`, `mod`, `quot` and `rem`, whose divisor must be a non-zero literal. A `case` must be exhaustive, as checked by the existing coverage warning. Recursion counts as possible non-termination: strict lists are finite, but proving that a recursive function terminates is not attempted.

In `lists`, every stage is total. In `queens`, only `safe` can fail, since it is recursive. Both fuse.

## The pipeline

A pipeline is a chain of saturated calls, outermost first: an optional consumer, transformers, and a producer.

| Role | Functions | Loop state |
|-|-|-|
| producer | `enumFromTo` and `enumFromThenTo` at `Int`, `replicate`; any other list expression is a source, traversed by `case` | a counter, or the remaining list |
| transformer | `map`, `filter`, `concatMap` / `concat (map f ...)` whose function returns a pipeline, `take`, `takeWhile` | `take`: a counter; `concatMap`: the inner pipeline's state and the outer state |
| consumer | `foldl`, and wrappers over it: `sum`, `product`, `length`, `and`, `or`, `any`, `all`, `elem` | the accumulator |

A function whose body is one saturated call to a pipeline function, with its list parameter in the list position, is unfolded first: `sum xs = foldl (+) 0 xs`, `concatMap f xs = concat (map f xs)`. Each wrapper then needs no rule of its own. Functions are recognized by their prelude definition, not their name, so a user function named `map` is not touched. Type-class clones such as `sum@Int` keep a pointer to the definition they were cloned from.

A pipeline with no consumer is materialized: the loop returns `x : go ...`, which TRMC turns into a list-building loop. `queens` is one: its result is returned to `length` in another function.

### Generated code

The loop is a local recursive function over the stages' state, placed where the pipeline was. `lists`' `round_` becomes:

```haskell
round_ n =
  case 1 of i0 -> case n of i1 ->            -- producer bounds, once, in order
  let go i acc = if i > i1 then acc
                 else if even i then go (i + 1) (acc + i * 3)
                 else go (i + 1) acc
  in go i0 0
```

`queens`' `go k` becomes a state machine over the inner counter `q`, the current board `qs`, and the rest of the outer list `bs`:

```haskell
go k = case go (k - 1) of src ->
  let loop q qs bs =
        if q > n then (case bs of [] -> []; (qs' : bs') -> loop 1 qs' bs')
        else if safe q qs then (q : qs) : loop (q + 1) qs bs
        else loop (q + 1) qs bs
  in case src of [] -> []; (qs : bs) -> loop 1 qs bs
```

Rules for the generated code:

- **Stage functions are evaluated once,** in the original argument order, before the loop. An argument that is already a value (a lambda, a global name, or a partial application of a global to variables or literals) is substituted into the loop. `ho_specialize` then makes it a direct call. Any other expression is bound strictly with `case` and called through the variable.
- **Each stage's result is bound strictly,** as `case f x of y -> ...`, because `map` evaluates `f x` before the rest of the list.
- **Types are attached to every generated expression,** from the types of the calls they replace, so `FK_CHECK_TYPES=1` stays clean.
- **Both backends run the fused code,** because the pass rewrites funk expressions. The interpreter therefore shows the same semantics as compiled code.

### Placement

The pass runs after elaboration and before `lift_locals`, so the loop is lifted, specialized and compiled like a hand-written one. `reuse_cells` then runs on what remains. It goes in a new file, `src/fuse.c`. The traversal helpers it shares with `elab.c` (`Nm`, `pat_nm`, `copy_expr`) move to `funk.h`.

## Tests

- **Reference semantics.** `FK_NOFUSE=1` disables the pass. The expected output of `tests/run/fuse.fk` is produced with it, so the test checks the fused program against the unfused one, in both backends.
- **Must fuse:** the two benchmark pipelines; `sum`, `length`, `any` and `elem` over `map` and `filter`; `concatMap` with an inner pipeline; `take` after total stages. `tests/run/fuse.fuse` lists the generated loops, as `reuse.reuse` lists reuse copies.
- **Must not fuse, with output equal to the unfused program:**
  - `mapM_ print` as a consumer, and a `map` whose function prints;
  - `map (div 1)` after a `filter` with a non-exhaustive `case`, where both can fail;
  - `take 2 (map f [1, 2, 0])` with `f 0 = error ...`;
  - a stage function in a local variable;
  - a user function named `map`.

## Expected effect

- `queens`: about 2.9x to 1.6x, as measured by hand.
- `lists`: depends on decision 2.
- `strings`: `map show (enumFromTo 1 n)` is materialized without the `Int` list. A small gain, not measured.

Geometric mean, from 1.68x: about 1.58x with `lists` unchanged; about 1.48x with `lists` compared against a fused C loop (about 1.3x). Estimates.

Size: about 500 lines in `src/fuse.c`, and tests. An estimate.

## Result

Implemented in `src/fuse.c`, about 1,000 lines. Differences from the design:

- **Placement.** The pass runs after `lift_locals`, not before, so local helpers such as `safe`'s `check` are globals the analyses can see. `lift_locals` then runs again on the bindings that got a loop.
- **Unfolding.** Only prelude definitions are unfolded. Unfolding user functions moved their pipelines into callers, duplicating code for no gain.
- **Scope.** One `concat` per pipeline, and none inside its inner pipeline. `enumFromThenTo` is not a producer. A `concatMap` whose outer element type is not `Int`, `Bool`, `()` or a list does not fuse: the loop's inner parameters start with a placeholder value of that type.
- **Desugared clauses.** `map f (x:xs)` binds `f` again in a `case`; the analyses treat such a pattern variable as the parameter it aliases.
- **Dictionaries.** A call through a dictionary counts as unknown, so overloaded code fuses only in its specialized clones.
- **Tests.** `tests/run/fuse.fk` and `fuse_err1.fk`, `fuse_err2.fk`, with expected output from `FK_NOFUSE=1`; `fuse.fuse` lists the functions that get a loop. The `.reuse` check now runs with fusion off, since fusion removes the lists it tests.

| Benchmark | Before | After |
|-|-|-|
| `queens` | 2.9x | **1.4x** |
| `pipeline` (old `lists`, against a C loop) | 2.6x against a list program | 1.3x |
| `lists` (new: named lists, no fusion) | -- | 2.3x |
| geometric mean | 1.68x over 10 | **1.52x** over 11 |

`queens` beats the hand-fused version (1.6x): the generated loop is a single TRMC loop, where the hand version recursed on the outer list.

## Decisions (taken)

1. **Exact semantics, as above, or imprecise errors?** Taken: exact. The rules above fuse only when the result is unchanged. The alternative is to declare that a pipeline that can fail in several stages may raise any one of their errors. This is Haskell's semantics for imprecise exceptions ([Peyton Jones et al., PLDI 1999](https://dl.acm.org/doi/10.1145/301618.301637)). It would drop rule 2 and fuse more pipelines, at the cost of a documented loss of determinism in error messages. Recommendation: exact. Both benchmarks fuse under it.
2. **What does `lists` measure after fusion?** Taken: (c).
   - (a) Rewrite `lists.c` as the loop a C programmer would write. The benchmark then measures what the compiler achieves on that program, and overlaps with `loop`.
   - (b) Change `lists.fk` so its lists cannot be fused, for example by using the filtered list twice. It then keeps measuring allocation and collection.
   - (c) Keep both versions as separate benchmarks.

   Recommendation: (c). `lists` becomes `pipeline`, against a fused C loop, and a new `lists` defeats fusion, so allocation stays measured.

The citations are from memory and were not checked in this session.
