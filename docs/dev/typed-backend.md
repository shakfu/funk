# Typed backend: design note

Status: proposal. It implements step 5 of [performance-first.md](performance-first.md). Decided there: the interpreter keeps tagged values while compiled code changes representation, and strings become a packed type (a separate note).

## Goal and evidence

Generated C should use C types where funk's types are known: `int64_t` for `Int`, `double` for `Float`, C truth values for `Bool`. Today every value is a tagged word (`V`), every `Float` is a heap box, and every call passes its arguments through a `V[]` array. Experiments on hand-edited generated C, on an M1:

| Program | Now | Typed by hand | C |
|-|-|-|-|
| `loop`, 40M iterations | 54 ms (inline primitives, tagged) | 22 ms (`int64_t` locals) | 21 ms |
| `fib 34` | 31 ms | 24 ms (typed parameters, direct calls) | 19 ms |

`floats` is 34x slower than C, and most of its time is spent allocating and collecting `Float` boxes. Its loop is monomorphic (`go :: Int -> Float -> Float`), so typed locals remove every box.

## Options

| Option | Representation | Polymorphic code | Cost |
|-|-|-|-|
| A. Full monomorphization (MLton) | every function cloned per instantiation; all values typed, data fields unboxed | none left | clones every polymorphic function; polymorphic recursion needs a fallback; largest change |
| B. Typed monomorphic code, uniform boundaries (chosen) | C scalars in monomorphic code; `V` in polymorphic code, data fields and unknown calls | unchanged: `V` | boxing where a scalar enters a `V` position |
| C. Unboxed fields only | data with `Float` fields stored raw | unchanged | no help for locals or calls |

B is chosen. The measured gaps, in `loop`, `fib` and `floats`, are all in monomorphic code. Type-class specialization already turns the common overloaded code into monomorphic clones (`fib@Int`, `sum@Int`). The other large gaps need different fixes, so A's extra cost buys little there:
- `lists` and `closures`: calls to unknown functions;
- `lazywhere`: thunks;
- `strings`: the string representation.

B also leaves the uniform representation in place for the one case A must work around, polymorphic recursion.

What B gives up, and the later steps that recover it:
- **Monomorphic data types with scalar fields** (`data Vec = Vec Float Float`) still box their fields. Step 5 lays such fields out unboxed: a field whose declared type is `Float` holds a raw `double` in every instance of the type.
- **Unconstrained polymorphic functions** (`map`, `foldl`) still exchange `V`. A `Float` passing through them is boxed. Specializing higher-order functions on known function arguments (TODO: unknown calls) is the place to clone them at scalar types too.

## Representations

| funk type | C type in typed code | Into `V` | Out of `V` |
|-|-|-|-|
| `Int` | `int64_t` | `FK_MKINT` | `FK_INT` |
| `Float` | `double` | `fk_float` (allocates) | `FK_FLOATV` |
| `Char` | `int32_t` | `FK_MKCHAR` | `FK_CHAR` |
| `Bool` | `bool` | `FK_BOOL` | `== fk_true` |
| `()` | `V` (`fk_unit`) | none | none |
| any other type, or a type variable | `V` | none | none |

A value's representation is decided by its type after defaulting, with clone substitutions applied (below). Any missing or variable type means `V`, so a gap in type information costs speed, never correctness.

The conservative collector stays correct. It may read a raw `int64_t` or `double` on the stack, or in a field, as a pointer, which only retains an object longer. The non-moving collector never writes through such words.

## What the code generator needs

1. **A type on every expression, lambda parameter, pattern variable and binding**, recorded by `check.c` during inference (today only integer literals carry one). Expressions created after inference get types where their creator knows them. That covers record lowering (the field type) and `do` lowering (the statement's type). The rest default to `V`.
2. **Concrete types in specialization clones.** A clone made by `elab.c` shares its original's `Type` objects, whose variables are the original's generic ones. `copy_expr` must substitute `tsub` into every recorded type, or `fib@Int`'s parameter would look polymorphic and stay `V`.
3. **Representation-aware expression generation.** `cg_expr` returns a C expression and its representation. A coercion is inserted only where the two sides differ: a `double` passed to a polymorphic function is boxed, and a `V` bound to an `Int` pattern variable is unboxed.
4. **Typed primitives on scalars.** `primAddInt` on two `int64_t` is `a + b`, with wraparound through unsigned arithmetic; `primLtFloat` on two `double` is `a < b`. The tagged `fk_i_` forms remain for `V` operands.
5. **Workers and wrappers.** A function whose type is monomorphic gets:
   - a *worker* with C parameters and return type, `static double fn_12_w(V self, int64_t a0, double a1)`;
   - the uniform *wrapper* `V fn_12(V self, V *a)`, which unboxes its arguments, calls the worker and boxes the result.

   Saturated known calls go to the worker. `fk_apply`, closures and partial applications use the wrapper. Self tail calls still become `goto top` over typed locals. A wrapper is emitted only if the function is used as a value; reachability already tracks this.
6. **Typed closure captures.** A free variable of scalar type is stored in its closure slot as raw bits, and the closure's code reads it back with its type, so capturing a `Float` does not allocate.

## Integers

Typed code computes in `int64_t`, so `Int` becomes 64 bits in compiled code. The interpreter keeps 63-bit tagged integers, and the two backends then disagree on overflow: `2^62` is `-2^62` interpreted and `2^62` compiled. Two ways to settle it:

- **Decided:** `Int` is 64 bits, and values that do not fit in 63 bits are boxed in a `K_INT64` object. Both backends then agree.
- Keep 63-bit semantics in compiled code by sign-extending after each `+`, `-` and `*`. Rejected under the performance goal.

Implemented in step 6, both were measured, A/B against the tree before the step, 11 benchmarks:

| | Geometric mean | Largest costs |
|-|-|-|
| 64-bit, boxed beyond 63 bits | +5.4% | `pipeline` +28%, `lists` +15%, `queens` +7%, `closures` +7% |
| 63-bit wrap in typed code | +29.3% | `pipeline` +133%, `loop` +118%, `lazywhere` +76%, `fib` +50% |

The decision assumed only the interpreter would box. The runtime is shared, so compiled code boxes too, wherever an `Int` enters a `V`: list elements, polymorphic calls, closures. Every such crossing now checks the range, and every exit from a `V` branches on the tag. The 63-bit wrap is one instruction (`sbfx` on ARM64), but it stops clang from reasoning about integer loops: `loop` is no longer vectorized and `fib` loses accumulator recursion elimination. A first measurement of the 63-bit option showed 0%; its build had not picked up the change, because the source was edited within the same second as the copy it was built from.

## Steps

Each step is measured with `bench/bench.py report` before the next.

1. ~~Record types (check.c, elab.c clones).~~ Done. Every expression, lambda parameter, binding and non-wildcard pattern has a type; dictionaries have the type `$Dict`. Clones substitute their types (`fib@Int,Int: Int -> Int`). `FK_CHECK_TYPES=1` reports gaps, and `make test` runs it on every run test and example; `FK_CHECK_TYPES=2` prints each top-level function's parameter and result types.
2. ~~Typed locals and expressions within a function body, with coercions at calls, fields and returns.~~ Done. Geometric mean 4.91x to **4.02x**: `loop` 2.6x to 1.0x, `floats` 34.3x to 11.4x, others 0-9% faster. `floats`' loop is still allocating, but for a thunk: `let x = ...` is lazy, which makes eager `let` the next cost there. Function boundaries are still `V`: parameters are unboxed once at entry and results boxed at `return`.
3. ~~Workers and wrappers, direct typed calls.~~ Done. Geometric mean 1.52x to **1.37x**: `fib` 1.5x to 1.0x, `queens` 1.4x to 1.1x, `trees` 2.2x to 1.7x, `sort` 2.2x to 1.8x. Every lambda is a worker `w_N` with C parameters and result, plus the wrapper `fn_N(V self, V *a)` for closures, thunks and `fk_apply`. Notes:
   - clang now turns some non-tail recursions into loops: `1 + f (n - 1)` runs in constant stack when compiled, but not when interpreted. `tests/run/overflow.fk` uses `1 + 2 * f (n - 1)`, which clang cannot turn into a loop.
   - Inlining is left to clang. Marking looping workers `noinline` made `pipeline` 2x faster, because clang vectorizes the loop when it is out of line, but `queens` 14% slower. It was not kept.
   - Copies made by `ho_specialize` keep the original's generic parameter types, so `flip@{0=primMulInt}` still takes `V`.

   The original expectation was `fib` near C. A hand prototype (2026-10-25) shows they also give the effect of an inliner: with `even`, `flip (*) 3` and `compare` at `Int` as typed C functions, clang inlines them. `pipeline` at n = 30M went from 192 ms to 97 ms, which is C's 96 ms. `sort` went from 77 ms to 70 ms; inlining by hand gave the same. A funk-level inliner is therefore not needed for these. Matching on the inlined `compare`'s result without building the `Ordering` (case-of-case) gave a further 5% on `sort` (66 ms). clang cannot do that itself, because `fk_con_value` is an out-of-line call.
4. ~~Typed closure captures.~~ Done. A free variable of scalar type is stored raw in its closure slot; a table computed before generation gives each lambda's free-variable representations, from the slots of the function creating the closure. Building 2M closures that capture a `Float` went from 83 to 77 ms. The benchmarks do not move: they capture three `Int`s in total, and boxing an `Int` is a shift.
5. ~~Unboxed scalar fields in constructors whose field types are concrete.~~ Done, for `Float`: `Int`, `Char` and `Bool` are immediate words already. A field declared `Float` holds the double's bits. The generated program registers each constructor's mask with `fk_con_raw` at startup, and `show`, `fk_compare` and constructors applied as function values consult it; the interpreter never registers one, so it keeps boxed fields. Building, keeping and summing 2M `Vec Float Float` values went from 67 to 24 ms. The benchmarks have no such type and do not move (A/B within 1%).
6. ~~64-bit `Int` in both backends (`K_INT64`), with tests at the 63-bit boundary.~~ Done; see "Integers". `tests/run/int64.fk` crosses the boundary in typed code, in list elements and between the two; the compiled code before this step got 10 of its 11 lines wrong. Geometric mean 1.37x to 1.48x.

Size: about 700-1,000 lines in `cgen.c`, which is 435 today, and about 150 in `check.c` and `elab.c`. These are estimates.

## Expected effect on the metric

The geometric mean is 4.91x. If steps 2-3 bring `loop` and `fib` to about 1.1x, as in the experiments, and `floats` to about 2x, the mean becomes about **3.2x**. This is an estimate, not a measurement; `floats` is the least certain of the three. The other planned steps then address the rest: eager `let` for `lazywhere`, higher-order specialization for `lists` and `closures`, and packed strings.

## Risks

- **Types lost in transformations.** Each pass that builds expressions after inference (records, `do`, elaboration, specialization) must carry types, or the code falls back to `V`. The fallback is safe but silent. A debug flag that counts `V` slots of concrete type in generated code would make such losses visible.
- **Two representations of one value.** A scalar may be needed both raw and boxed, for example an `Int` used in arithmetic and also stored in a list. Generation keeps the raw form and boxes at each use in a `V` position. Repeated boxing of one `Float` allocates repeatedly; caching the boxed form per variable avoids that. Measured after `ho_specialize` copies got concrete types (2026-10-25): `merge`'s `x` and `y` became `int64_t`, so each output cell re-boxes them, with step 6's range check, and `sort` got 2.5% slower. Keeping the matched word next to the unboxed copy, for pattern variables, fixed `sort` but made `queens` 32% and `lists` 7% slower, probably from register pressure in hot loops. It was not kept.
- **Code size.** Workers and wrappers add C code for every monomorphic function used as a value. Reachability limits this to functions actually referenced.
- **Divergence between backends.** The interpreter is now the reference only through the shared front end and the tests. Every run test already executes under both backends against one expected output, which remains the check.

The MLton comparison is from memory and was not checked in this session.
