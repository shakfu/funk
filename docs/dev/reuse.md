# In-place reuse of list cells: design note

Status: implemented, released in 0.2.0; see "Result". Elements, tuple components and cells outside TRMC sites: [element-reuse.md](element-reuse.md), whose ownership analysis replaced `fresh` and `consumes` below. It follows the Perceus prototype in [memory.md](memory.md#perceus-prototyped-by-hand). Writing `filter`'s and `map`'s output into their input's cells made `lists` 18-23% faster (69 ms to 53-57 ms, with only those two functions changed). Freeing cells made it slower, because it scatters later allocations. This note gets the reuse without reference counts, from a static check.

## The rule

A call may overwrite the cells of an argument's spine when two things hold:

1. **The argument is fresh:** no other reference to its spine exists, or will exist.

2. **The parameter is consuming:** the callee takes the value apart once, and keeps no reference to the cells it has taken apart.

The spine is the chain of cells through a list's tail fields. Elements are never modified. A reused cell only gets a new reference in its head field, so an element shared with other data is unaffected.

### Fresh arguments

An argument expression is fresh if it is one of:

- a saturated call to a fresh-producing function of arity 1 or more;

- a cons cell or list literal whose tail is fresh, or `[]`.

A reference to a top-level value of arity 0, such as `nums = enumFromTo 1 10`, is never fresh: it is evaluated once and shared by every use.

A variable is never fresh, even one bound to a fresh value: in `span p xs = (takeWhile p xs, dropWhile p xs)`, both calls receive `xs`, and reusing it in the first would corrupt the second. Version 1 has no exception for variables used once; that would need a use count over the variable's scope.

A function is **fresh-producing** if every result it returns in tail position is one of: `[]`; a fresh expression (`filter`'s skip branch, `filter p xs`); or a cons cell whose tail is one of these. The set is computed as a greatest fixpoint over all global functions: start by assuming every function qualifies, and remove each one that returns anything else, until nothing changes. In the prelude: `map`, `filter`, `take`, `takeWhile`, `zip`, `zipWith`, `replicate`, `enumFromTo`, `init`, and their specialization copies. Not fresh: `drop` and `dropWhile`, which return a suffix of their input; `++`, whose result ends in `ys`; `merge`, which can return either input.

### Consuming parameters

Parameter `p` of function `f` is consuming if, on every path through `f`'s body:

- `p` is used at most once: as the scrutinee of a `case` (clauses desugar to exactly that: `\$a0 -> case $a0 of ...`), or as the argument of another consuming parameter (`length xs = foldl ... xs`).

- In an alternative matching a cons cell `(x : rest)`, the matched cell is not used again. `x` may be used freely. `rest` is used at most once, and only as the argument of a consuming parameter: a self call in the same position, or a call to another function at a consuming parameter.

- An alternative that binds the whole value to a variable (`xs` in `filter p xs`) counts that variable as `p`: the same rules apply to it.

For example, `map`'s and `filter`'s list parameters are consuming, and `foldl`'s and `length`'s are too. `span`'s is not, since it passes `xs` twice.

## The rewrite

- **Copies.** For a call `f ... e ...` where `e` is fresh at a consuming parameter `k`, the call goes to `f@reuse{k}`, a copy of `f` marked to reuse that parameter's cells. Inside the copy, the recursive call passes `rest`, which belongs to the same fresh spine, so it stays in the copy.

- **Reuse.** In the copy, on a path that matched cons cell `C` of parameter `k`, the first allocation of a cons cell in tail position takes `C`'s memory instead of allocating. The matched fields are already in locals by then. In TRMC loops, that allocation is the output cell: `map` writes `f x` into `C`'s head, and `filter` keeps `C` on the path that keeps `x`.

- **No freeing.** A cell that is not reused (`filter` skipping an element, `foldl` consuming its list) is left to the collector, which keeps allocation in address order.

- **Backends.** The copies are ordinary functions, so the interpreter runs them unchanged and simply allocates. Only `cgen.c` emits the reuse. Run tests compare both backends, so a wrong reuse shows up as a difference in output.

### Reconstruction

`merge (x:xs) ys` rebuilds the cell it just matched. Rewriting a constructor application whose fields are exactly the variables of the pattern just matched into the matched value itself is always safe, since cells are immutable, and needs no freshness. It is a separate rewrite in the same pass. In the prelude it applies to `merge` (`sort`) and `dropWhile`.

## Safety

The collector stays correct. A reused cell is still a valid object, and the only references to it are the output's. A stale copy of an input pointer in a dead stack slot can keep a cell alive longer, which is harmless.

A rewrite is wrong only if a reference to a reused cell survives, and the two conditions exclude every way one could:

- **From the caller:** the argument is a direct call or literal, not a variable or a top-level value, so no one else holds a reference.

- **From the callee:** a consuming parameter's matched cell is never used again, and its tail is passed only to consuming parameters.

- **From the producer:** a fresh-producing function returns only cells it allocated itself.

- **From errors:** an error raised partway through can only leave the input half rewritten, and the input is unreachable after the call.

### Tests

`tests/run/reuse.fk`:

- Programs that must not reuse, each checked against the interpreter's output:
  - `let ys = filter p xs in (map f ys, ys)` (a variable used twice);

  - `span`;

  - a function returning its argument alongside a map of it;

  - `xs ++ ys` passed to `map`;

  - a top-level list value passed to `map`, then used again.

- Programs that must reuse. `tests/run/reuse.reuse` lists the reuse copies the emitted C must contain, exactly, so a copy that should not exist also fails. It replaces the allocation counter planned here: it checks the same decisions without a runtime flag.

## Result

Implemented as designed, at the end of `elab.c`, plus about 15 lines in `cgen.c`. Differences from the design:

- **Copies only where they pay.** A call is redirected only if the callee has a TRMC site, where cgen can write into a cell. Since [element-reuse.md](element-reuse.md)'s phase 1, any allocation of the parameter's cell size counts. `foldl` and `length` are consuming, but a copy of either would allocate the same.

- **Type-class clones are visible.** Clones such as `enumFromTo@Int,Int` had no `Bind` registered with their global. The pass registers it, or their results would not count as fresh.

| Benchmark | Before | After |
|-|-|-|
| `lists` | 3.1x | 2.6x |
| `sort` | 3.0x | 2.2x |
| `queens` | 3.0x | 2.9x |
| geometric mean | 1.79x | **1.68x** |

`sort` gains the most, from reconstruction in `merge`, which allocated a cell per step to rebuild its argument. The estimate was 1.72x.

## Expected effect

`lists`: about 20% faster, as measured; `queens` and `sort` somewhat, from `map`/`filter` chains and from reconstruction in `merge`. The geometric mean would move from 1.79x to about 1.72x. This is an estimate.

## Size

About 300 lines: the freshness fixpoint and the consuming check in `elab.c`, after specialization; the reuse in `cgen.c`'s TRMC path; tests. An estimate.
