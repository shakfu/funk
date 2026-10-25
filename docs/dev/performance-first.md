# Performance first: design review

Status: proposal, needs decisions (end of note). It revisits every design decision under one rule: the C that `funk build` generates must be as fast as possible. A decision that slows generated code needs a reason that outranks this rule, and the reason must be stated.

## The metric

The geometric mean, over the suite in [bench/README.md](../../bench/README.md), of compiled funk's time divided by C's time. **The target is 2x.** Today it is **10.0x**, on an M1:

| Benchmark | Ratio | Benchmark | Ratio |
|-|-|-|-|
| `closures` | 2.6x | `sort` | 12.6x |
| `trees` | 3.3x | `strings` | 16.1x |
| `fib` | 3.7x | `lazywhere` | 22.0x |
| `loop` | 5.5x | `lists` | 22.0x |
| `queens` | 9.4x | `floats` | 62.4x |

The C programs use the same algorithms and linked data structures (see the bench README), so the ratio measures the compiler and runtime, not the choice of data structures.

## Where the time goes

Measured by editing the generated C by hand, or by sampling with `sample(1)`. Compiled with `cc -O2` on an M1.

| Experiment | Time | C |
|-|-|-|
| `loop` as generated: every `+`, `-`, `mod`, `==` is a call to an `rt.c` primitive; `==` goes through `fk_compare` | 116 ms | 21 ms |
| `loop`, primitives inlined on tagged words | 53 ms | |
| `loop`, unboxed `int64_t` locals | 22 ms | |
| `fib` as generated: arguments passed in a `V[]` array, primitives called | 74 ms | 19 ms |
| `fib`, primitives inlined on tagged words | 32 ms | |
| `fib`, direct C parameters, unboxed `int64_t` | 24 ms | |
| `fib`, as above without `FK_STACK_CHECK` | 20 ms | |

Samples of the worker thread, as shares of its samples:

| Benchmark | GC and allocation (`fk_alloc`, `fk_gc`, `scan_range`, `memset`) | funk code |
|-|-|-|
| `floats` | 88% | 12% |
| `lists` | 70% | 20% |
| `trees` | 70% | 30% |

Conclusions:

1. Unboxed scalars with direct calls reach C speed on `loop` and `fib`. The gap there is representation and calling convention, not the language.
2. Allocation-heavy code is dominated by memory management. `floats` allocates a box per `Float` result; `lists` and `trees` spend most of their time scanning and sweeping.
3. The per-call stack check costs about 20% on call-heavy code.

## Decisions revisited

Verdicts: **keep**, **change**, or **decide** (needs the user).

| Decision (design.md) | Effect on generated code | Verdict |
|-|-|-|
| Strict evaluation | the fastest choice; no thunks for arguments | keep |
| Lazy `let`/`where` values | one thunk per binding: `lazywhere` 22x, 6x slower than strict `where` | change: evaluate eagerly when the right side is pure, total and cheap; thunk only otherwise |
| One tagged representation shared by both backends | every `Int` op is a call or a tag operation; every `Float` is a heap box | change: the compiled backend uses types: `int64_t`, `double` and unboxed fields; the interpreter keeps tagged words |
| Dictionary passing, specialization at known call sites | dictionaries remain in code that is not specialized | change: the C backend sees the whole program, so it monomorphizes fully. Dictionaries stay in the interpreter, and as a fallback for polymorphic recursion |
| Uniform calling convention `V f(V self, V *args)` | arguments go through memory; no unboxed parameters | change: direct C functions with typed parameters; the uniform entry is only a wrapper for unknown calls |
| `FK_STACK_CHECK` in every function | 20% on `fib` | change: a guard page under the 4 GiB stack, with a `SIGSEGV` handler on an alternate signal stack. The check is removed |
| Conservative, non-moving mark-sweep GC | 70-88% of allocation-heavy time | change, in two steps; see below |
| Strings as `[Char]` | 16x on `strings`; 3 words per character | decide |
| 63-bit tagged `Int` with wraparound | a shift and mask per operation | change: unboxed `int64_t` gives full 64 bits; this answers the roadmap's open question |
| Records lowered to `case` | becomes a single load once layouts are known | keep |
| Type classes at the source level | no cost once monomorphized | keep |
| `do` desugared to `case` | no cost; keeps tail position | keep |
| Exhaustiveness warnings, fixities, deriving | compile time only | keep |
| Typed effects ([typed-effects.md](typed-effects.md)) | the recommended B1 runner allocates per action | change the recommendation to A'. If `IO` is wanted, it must compile to direct calls |
| Interpreter and REPL sharing `rt.c` | ties the compiled representation to what the interpreter can run | decide |
| Prelude compiled into every program | code size; placement alone moves `loop` by 13% | change: drop unreachable code |

### Memory management

Two steps, measured between them:

1. **Fix the collector's hot paths.** `mark_word` searches the chunk table for every word, tagged integers included. Tagged words should be rejected first: integers have their low bit set. Sweep zeroes every dead object with `memset`, which is 461 of the `floats` samples; zeroing at allocation, or not at all, is enough. Cost: tens of lines.
2. **A bump-allocated nursery with copying collection**, keeping conservative stack roots by pinning the pages they point into (Bartlett's mostly-copying collector). Short-lived cons cells and boxes, which most of this allocation is, then cost a pointer increment and are never swept. Typed layouts from monomorphization let the collector scan the heap precisely.

The alternative to step 2 is reference counting with reuse (Perceus, as in Koka and Lean 4; [Reinking et al., PLDI 2021](https://dl.acm.org/doi/10.1145/3453483.3454032)). A uniquely owned cell is updated in place, so `map f xs` on a list used once allocates nothing. Its costs: count operations in all generated code, and cycle handling for recursive closures. This needs its own design note before choosing.

### Precedent

MLton compiles Standard ML through whole-program monomorphization, defunctionalization and unboxed representations, and comes within a small factor of C. funk's C backend already sees the whole program, so it can take the same route. The interpreter keeps what it needs for the REPL.

## Plan, by payoff per cost

1. **Inline primitives at known types in `cgen.c`**, on tagged words: `loop` 2.2x faster, `fib` 2.3x. Small.
2. **GC hot-path fixes.** Small; to measure.
3. **Guard-page stack overflow.** About 20% on call-heavy code. Small.
4. **Drop unreachable code.** Small.
5. **Typed backend:** full monomorphization in `cgen.c`, unboxed `Int`, `Float`, `Char` and `Bool`, typed layouts for data, direct calls with typed parameters. `loop` and `fib` reach C speed, and `floats` loses its boxes. Large: it changes how `cgen.c` works and needs a design note.
6. **Nursery or Perceus**, after 5, with its own design note.
7. **Eager `let`** for pure, total, cheap right sides. Medium.

Steps 1-4 are independent of the larger decisions and pay off immediately.

**Steps 1-4 done.** Geometric mean 10.26x to **6.58x** (same machine, same session for both runs' C programs):

| Benchmark | Before | After | Benchmark | Before | After |
|-|-|-|-|-|-|
| `fib` | 3.7x | 1.5x | `trees` | 3.3x | 2.6x |
| `loop` | 5.5x | 2.6x | `closures` | 2.6x | 2.3x |
| `queens` | 9.4x | 5.6x | `lazywhere` | 22.0x | 10.7x |
| `sort` | 12.6x | 11.1x | `strings` | 16.1x | 15.2x |
| `lists` | 22.0x | 18.0x | `floats` | 62.4x | 37.3x |

What remains is boxed `Float` and memory management (`floats`, `lists`, `strings`, `sort`) and thunks (`lazywhere`): steps 5-7. Step 5 changes the shape of `cgen.c`, so steps 1 and 3 should be written so that they survive it.

## Decisions needed

1. ~~What is the target?~~ Decided: a geometric mean of 2x.
2. **What may be sacrificed?** Each of these has a cost in speed:
   - Haskell semantics: lazy `let` (step 7 keeps it where it matters); `String = [Char]`; typed `IO`.
   - The interpreter: with full monomorphization, the interpreter and the compiled code no longer share a representation, and the shared runtime stops guaranteeing one semantics. The tests still run both backends against one expected output. Dropping the interpreter would mean a REPL that compiles each input with `cc` and loads it with `dlopen`, which was measured at 50-200 ms per input when it was rejected.
   - Size: the typed backend and a new collector add perhaps 2,000-3,000 lines. That is an estimate.
3. **Strings:** a packed builtin string type, faster than `[Char]` by more than the 16x gap, would make `"abc"` a different type from `['a', 'b', 'c']`. Prelude functions such as `words` and `lines` would need versions for it.
4. **Are compile time and memory use part of the goal?** Monomorphization grows code and compile time; the rule as stated does not count them.

The MLton claim and the citation are from memory and were not checked in this session.
