# Reuse of elements and of cells outside TRMC: design note

Status: phases 1-3 implemented (unreleased; see "Phase 1 result" and "Phases 2 and 3 result"); field reads not. It extends [reuse.md](reuse.md), which reuses a list's spine cells at TRMC sites.

## Problem

`bench/game/nbody.fk` is 2.8x C. It allocates nothing but constructor cells, per step of the simulation:

| Object | Per step | Words | Built by |
|-|-|-|-|
| `Body` | 25 | 200 | `pair` (20), `move` (5) |
| cons cell | 15 | 45 | `against` (10), `velocities` (5) |

Tuples were 25 more per step until tuple results ([architecture.md](architecture.md), C generator). The C program updates five structs in place and allocates nothing.

reuse.md reuses none of these cells, for three reasons:

1. **Only a list's spine is fresh.** Elements are never modified. The bodies are elements of the list `steps` passes along.
2. **A variable is never fresh.** In `case against dt b bs of (b1, bs1) -> b1 : velocities dt bs1`, `b1` holds the only reference to a body `pair` built, and is used once.
3. **Only TRMC sites reuse.** `pair` and `against` build their cells inside tuple results, and `move` returns a plain constructor.

## Experiment

The C emitted for `nbody.fk` was patched by hand to write each new cell into a dead one it had just matched (n = 10,000,000 steps, M1, interleaved runs, minimum of 7):

| Variant | Time | x C | Resident | Collections |
|-|-|-|-|-|
| today | 1.62 s | 2.8x | 134 MB | 190 |
| `pair` and `move` write into the bodies they matched | 0.94 s | 1.6x | 135 MB | 26 |
| also `against` and `velocities` into the cons cells they matched | 0.59 s | 1.0x | 2 MB | 0 |

All three print the same energies. In this program every cell is unique, so the patches need no check; they bound what the design below can reach.

## The rule

reuse.md's two conditions stay: a call may overwrite cells of an argument if the argument is fresh and the parameter is consuming. Both are widened.

### Fresh values

A value is **fresh** if no other reference to its cell exists. A list is **fresh to its elements** if its spine is fresh and so is every element. A tuple result is fresh per component.

An expression is fresh if it is one of:

- a constructor application, of any type;
- a saturated call to a fresh-producing function (below);
- a **linear variable**: bound by a pattern on a fresh value, such as a component of a fresh tuple or an element of a list fresh to its elements, and used exactly once on each path, not in a guard, a lambda or a lazy `let` (which would capture it);
- `[]`, a cons cell whose tail is fresh (spine), or whose head is fresh too (elements).

A function's result summary is computed as a greatest fixpoint, as reuse.md's fresh-producing set: for each function, whether its result cell is fresh, whether a list result is fresh to its elements, and, for a tuple result, which components are fresh. In `nbody.fk`: `pair` returns two fresh components; `against` a fresh body and a list fresh to its elements; `velocities` and `advance` lists fresh to their elements. `replicate` is fresh in its spine only, since its elements are its argument.

### Consuming parameters

A parameter of any data type is **consuming** if, on every path, it is matched by at most one `case` and its cell is not used afterwards; its fields may be. For a list, it is **consuming to its elements** if, in addition, each element bound from it is used linearly: matched and dead, or passed once to a consuming parameter.

`pair` consumes both bodies. `move` consumes its body. `against` consumes its first argument and its list to the elements; so do `velocities` and `map` at `move`.

Field selections need one addition. `records`' `step p` reads `p.px` and three more fields before `p { ... }` rebuilds `p`. Each selection is a `case` on `p`. It counts as a read, not a use, if it comes before the reusing allocation in evaluation order. Since evaluation is strict, that holds when the reads are arguments of the allocation or come before it on the same path.

## The rewrite

- **Copies.** As in reuse.md, a call passing a fresh value at a consuming parameter calls a copy. Copies are keyed by parameter and mode: `f@reuseK` (the spine, as today) and `f@ownK` (the cell, and a list's elements). Inside an `@own` copy, a variable bound to an element of the owned list is linear, so passing it on selects further `@own` copies: `velocities@own1` calls `against@own1@own2`, which calls `pair@own1@own2`.
- **Reuse sites.** In a copy, on a path that matched a dead cell `D` of `n` words, the next allocation of a constructor of `n` words takes `D`'s memory: cgen emits a write of the header and the fields into `D` instead of `fk_mkcon`. This covers every constructor allocation, in tail position or not, inside a tuple result or not. TRMC output cells (reuse.md) become one case of it. Each dead cell serves one allocation.
- **Same size only.** The chunk a cell lives in holds objects of one size, so a 3-word cons cell cannot become an 8-word `Body`.
- **Interpreter.** As today, it runs the copies unchanged and allocates.

## Safety

reuse.md's argument carries over: a reused cell is overwritten only when no reference to it survives. The additions:

- **Linear variables.** A variable used once, not captured, is the only reference to its value, since the value was fresh when bound. A lazy `let` or a lambda would keep a second reference, so either disqualifies it.
- **Shared elements.** An element is fresh only if the producer built it. `replicate 3 b`'s elements are one cell referenced three times; its result is fresh in the spine only, so no copy reuses its elements.
- **The collector.** During incremental marking, overwriting a pointer field deletes a reference, so the old value is shaded first (`FK_SHADE`), as reuse.md's head write does. Fields declared `Float` hold raw doubles and need no shade. Shading every non-raw field before the write also covers a cell whose constructor changes between two of the same size, since the raw-field mask comes from the constructor in the header. Marking runs only at allocations, and the write sequence allocates nothing, so no step sees a half-written cell.
- **Errors.** An error raised partway through can only leave an input half rewritten, and the input is unreachable after the call.

## Tests

- `tests/run/reuse.fk` gains cases that must not reuse, each checked against the interpreter's output:
  - a body used twice (`let b = ... in pair dt b b`);
  - a body captured by a lambda, then passed on;
  - `replicate` passed to `map` with a consuming function;
  - an element of a top-level list value;
  - a field read after the reusing allocation.
- `tests/run/reuse.reuse` lists the `@own` copies the emitted C must contain, as today.
- `FUNK_GCSTRESS` over every run test exercises the shading of overwritten pointer fields.
- `bench/game/nbody.fk` and `bench/progs/records.fk` must print unchanged output.

## Expected effect

- `nbody`: 2.8x to between 1.6x (bodies only) and 1.0x (bodies and cons cells), from the experiment.
- `records`: `step` writes into each particle, as `move` does; 1.1x today. An estimate.
- Programs that build data once and only read it (`trees`, `binarytrees`) gain nothing: no allocation follows a dead match.

## Size

About 350 lines in `elab.c` (result summaries, linear variables, `@own` copies, field reads) and 60 in `cgen.c` (reuse at any allocation, shading), plus tests. An estimate.

## Phasing

Each phase is testable alone; `nbody` needs all three.

1. **Reuse at any allocation of the same size**, for parameters of any data type that are consuming and passed fresh: constructor applications and fresh-producing calls. `move` still does not qualify, since its body is a list element.
2. **Linear variables:** `b1` from `pair`'s result.
3. **Elements:** lists fresh to their elements, and `@own` copies.

## Phase 1 result

Implemented in `elab.c` (`cell_sizes`, `alloc_sizes`, and `fresh`, `consumes` and `gain` widened) and `cgen.c` (`reuse_cell`), without field reads:

- **Data types.** A parameter of any data type can be consuming, and a constructor application is fresh in its top cell. `tests/run/reusedata.fk` reuses cells in `scale`, `turn` (which changes constructor), `swap` (pointer fields, shaded) and `rev`, whose cons cell is built as an argument.
- **Any allocation of the same size** takes the cell, once per path. After a non-tail `if` or `case` whose branch took it, no later allocation does; before, cgen restored it, which was safe only because TRMC sites end the path.
- **Copies where an allocation fits.** A copy pays if the body allocates a cell of the parameter's size; it was a TRMC site found by the function's own name. Specialized clones such as `filter@{0=even}` now get copies too: in `pipeline`, `queens` and `reuse.fk`. That they were missed before is inferred from the copies appearing, not traced.
- **Effect.** None measurable, as expected: both suites' geometric means moved by under 0.2% in interleaved runs, and `nbody` and `records` emit no new copy. Their bodies are list elements, which need phases 2 and 3; `records` also needs field reads.

## Phases 2 and 3 result

Implemented together in `elab.c`, as one ownership analysis that replaced reuse.md's `fresh` and `consumes`. It differs from the rule above:

- **Three modes:** unowned, spine (a list's cells, another value's top cell), unique (every cell reachable from it). A list of `Int`s is unique, so most copies are now `@ownK`; `@reuseK` is the spine mode.
- **Linear replaces consuming:** a parameter or pattern variable is linear if used at most once on each path, a use in a lambda counting twice. Returning it or storing it is allowed: a copy reuses only cells it matches, and one use leaves no second reference. `run k ps = if k == 0 then ps else run (k - 1) (map step ps)` returns `ps` on one path, so `run` was not consuming; now `run@own1` owns it, and `map step` reuses the list's cells (`records` -22.5%).
- **Versions.** A call's result mode depends on its arguments': `against`'s result is unique when its arguments are. Results are summarized per version, a function and its parameters' modes, as a greatest fixpoint that starts from unique and is lowered; every body, values such as `main` included, is scanned for the versions it calls. A copy pays if it allocates a cell an owned parameter could give, or passes owned values to a copy that pays: `steps` and `advance` allocate nothing themselves.
- **`let` values stay unowned:** a lazy `let`'s thunk keeps its value after forcing it.
- **cgen** keeps a pool of dead cells, since `pair` matches two bodies in one `case`, and a cell matched from a fresh call result is dead too. A rebuild with the matched constructor skips the fields it would write unchanged (`Node (insert k v l) k2 v2 r` writes one field).

Bugs found while measuring, both fixed with tests:

- **A type variable counted as having no cells.** In a polymorphic clone, an element's type is a variable, and `cell_sizes` gave it 0, so a list of one shared value was unique. `tests/run/reuseown.fk`'s `dup` reproduces it: with the old rule, `map (scale 2.0) (dup v)` printed `(8.0,16.0)` and garbage.
- **Copies hid the prelude from fusion.** The prelude's `concatMap` called `concat@reuse0`, which fusion did not recognize, so `queens` stopped fusing (+18.7%, 9 to 18 MB). Fusion now looks through a copy to the function it copies.

Effect, interleaved runs against the tree before phase 1, all 25 programs:

| Program | Change | Resident |
|-|-|-|
| `nbody` | -58.5% (2.8x to 1.2x C) | 134 to 2 MB |
| `records` | -22.5% | 135 to 122 MB |
| `bst` | +8.6% | 150 to 140 MB |
| others | within 2.2% | unchanged |

`bst`'s inserts now update the tree in place. Building takes the same time (0.191 s); its lookups take 27% longer, since path copying had kept moving the tree's upper levels into fresh, adjacent memory, and in place every node stays where it was first allocated. The core suite's geometric mean is -0.9%, the game suite's -8.3%.

## Alternatives

- **Reference counting (Perceus).** Rejected in [memory.md](memory.md#perceus-prototyped-by-hand): freeing scattered allocations and made `lists` slower, and counts cost increments throughout generated code. A one-bit count, set when a reference is copied, needs the same copy tracking in code generation.
- **Uniqueness or linear types in the source**, as Clean's `*T` or Linear Haskell's `a %1 -> b`. Precise and cheap to compile, but the programmer annotates, and funk's checker would gain a type discipline.
- **Mutable storage in the program.** `nbody.fk` written over `Array Float` would likely run near C, as spectral-norm does (0.8x); the pure version would stay at 2.8x.
- **Escape analysis.** `pair`'s results escape into the list, so stack or region allocation does not apply.

## Decisions needed

- Whether to proceed, and with phase 1 alone first.
- Whether field reads before a reusing allocation are in phase 1. They decide `records`, which needs them; `nbody` does not.
