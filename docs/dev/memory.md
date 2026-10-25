# Memory: design note

Status: proposal. It covers step 6 of [performance-first.md](performance-first.md): the cost of allocation and collection. The geometric mean is 2.4x against the 2x target. The largest ratios left are `lists` 8.0x, `queens` 5.4x, `sort` 4.7x and `strings` 4.0x, all allocation-heavy.

## Where the time goes

Samples of compiled `lists`, on an M1:

| Function | Share |
|-|-|
| funk code, allocation inlined | about 55% |
| `scan_range` (marking) | about 25% |
| `fk_gc` (mostly sweeping) | about 15% |

Most of the marking is the stack. `map`, `filter` and `enumFromTo` recurse once per element, so a list of 300,000 elements leaves 300,000 frames on the stack while it is built. Each collection scans all of them conservatively, word by word. The C program builds the same lists in loops, with a pointer to the last cell's tail.

## Experiment: building lists in loops

In the generated C for `lists`, `enumFromTo@Int`, `filter@{even}` and `map@{...}` were rewritten by hand as loops. Each allocates a cell with an empty tail, stores it through a destination pointer, and moves the pointer to the new cell's tail:

```c
V head = fk_nil, *dst = &head;
for (; FK_CONID(l) == FK_C_CONS; l = FK_FIELD(l, 1)) {
  V cell = fk_mkcon(FK_C_CONS, 2, (V[]){f(FK_FIELD(l, 0)), fk_nil});
  *dst = cell;
  dst = &FK_FIELD(cell, 1);
}
return head;
```

| | Time | Ratio to C |
|-|-|-|
| as generated | 176 ms | 8.0x |
| three functions as loops | 70 ms | about 3.1x |

The stack stays shallow, so marking and the per-element call overhead mostly disappear. This is tail recursion modulo cons (TRMC). OCaml has it as `[@tail_mod_cons]` ([Bour, Clément and Scherer, 2021](https://arxiv.org/abs/2102.09823)), and Koka applies it automatically.

## Options

| Option | Measured or expected | Cost | Note |
|-|-|-|-|
| A. TRMC in `cgen.c` (chosen first) | `lists` 2.5x faster, measured by hand | about 150 lines; code generation only | also removes the stack limit for `map`-like functions |
| B. Bump-allocated nursery, mostly-copying minor collections | allocation becomes a pointer bump; short-lived cells are never swept | large: copying with pinned pages for conservative roots; a write barrier where young values are written into old objects (thunk updates, TRMC destinations) | the stack is still scanned at each minor collection, so it needs A to pay off |
| C. Perceus reference counting with in-place reuse | `map` over a list used once allocates nothing | very large: reference-count operations throughout generated code, the interpreter and the runtime; cycles from recursive local closures must be removed by lambda lifting | the largest gain, the largest change |
| D. Fusion (`foldr`/`build`) | `sum (map f (filter p xs))` becomes one loop with no list | large: rewrite rules and inlining | removes the lists that the C benchmarks keep; helps only pipelines |

A comes first. It is measured, small and independent, and it makes B cheaper, since B's minor collections would otherwise scan the same deep stacks. B or C is decided after A, from a new profile.

## TRMC: design

**Candidates.** A function is a candidate if, in some tail position, its result is a constructor application with a saturated self-call as one argument: `x : map f xs`, `merge a b : mergePairs rest`. In the prelude this covers `map`, `filter`, `++`, `takeWhile`, `enumFromTo`, `enumFromThenTo`, `zip`, `zipWith`, `replicate`, `take`, `init`, `mergePairs` and `merge`, plus their specialization copies.

**Evaluation order.** Evaluation is strict, left to right. In `C a (f xs) b`, `b` is evaluated after the recursive call. A loop would evaluate it before. The transformation therefore applies only if every argument after the self-call is safe to evaluate early, by the same rule as eager `let`. For `x : f xs`, the self-call is the last argument, so this never limits it.

**Code.** The function keeps its signature. In its body:
- Two locals: `head`, which receives the first result, and `dst`, which points to where the next result goes. `dst` starts as `&head`.
- A tail-position constructor application with the self-call allocates the constructor, with a placeholder in the self-call's field, and stores it through `dst`. It then points `dst` at that field and jumps to the top with the self-call's arguments, as self tail calls already do.
- Every other tail-position result is stored through `dst`, and the function returns `head`.

Self tail calls in other branches, such as `filter` skipping an element, keep jumping to the top with `dst` unchanged.

**Collector.** The non-moving collector sees `head` on the stack, and every cell is reachable from it through tails that have been written. `dst` is an interior pointer, which the collector already accepts. A collection between allocating a cell and the loop's next store finds the placeholder, which is an ordinary value. Option B would need a write barrier on the store through `dst`, because the cell may have been promoted in between.

**Semantics.** The results are unchanged. Two behaviours change for the better: a long `map` no longer uses stack proportional to its length, so it can no longer overflow; and an error raised partway through leaves only an unreachable partial list.

**Interpreter.** Not changed. It still recurses, which is correct and only slower; interpreter speed is not part of the goal.

## Expected effect

If `lists` reaches about 3.1x, as measured, and `sort`, `strings` and `queens` gain a third as much, the geometric mean moves from 2.4x to about 2.1x. This is an estimate. Profiles after TRMC decide between B and C.

The OCaml citation is from memory and was not checked in this session.
