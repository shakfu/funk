# Architecture

How funk 0.1 is built. The reasons behind each choice are in
[design.md](design.md).

## Pipeline

```
source --lex.c--> tokens (layout applied)
       --parse.c--> AST, desugared
       --check.c--> SCC groups, inferred types
       --resolve.c--> frame slots, free-variable lists
           |--interp.c--> evaluate (REPL, funk run)
           '--cgen.c----> C source --$CC--> executable
```

Both backends link the same runtime, `rt.c`.

| File | Lines | Role |
|-|-|-|
| `src/lex.c` | 328 | tokenizer, layout rule |
| `src/parse.c` | 989 | recursive-descent parser, desugaring |
| `src/check.c` | 879 | data types, SCC analysis, type inference, chunk driver |
| `src/resolve.c` | 137 | closure conversion |
| `src/interp.c` | 160 | interpreter |
| `src/cgen.c` | 293 | C generator |
| `src/rt.c`, `rt.h` | 842 | values, GC, apply, `show`, compare, primitives |
| `src/prims.def` | 39 | primitive table: C name, funk name, arity, type |
| `src/main.c` | 284 | CLI, REPL, `cc` invocation |
| `src/util.c`, `funk.h` | 436 | allocation, interning, maps with undo, AST types |
| `lib/prelude.fk` | 180 | standard functions in funk |
| `tools/embed.c` | 35 | embeds `rt.c` and the prelude into the binary |

## Lexing and layout (`lex.c`)

The tokenizer marks each token that starts a line. A second pass applies
the layout rule with a context stack. Each implicit context records:

- its column;
- the bracket depth at which it opened (`)`, `]` and `,` close it);
- pending `if` count (`then` and `else` close inner blocks);
- whether it is a `let` block, plus a count of `let` blocks closed before
  their `in` arrived. This stops `in` from closing an enclosing block after
  indentation already closed the `let`.

Virtual braces (`T_VLBRACE`, `T_VRBRACE`) are distinct token kinds, so an
explicit `}` cannot close a layout block.

## Parsing and desugaring (`parse.c`)

Expressions use precedence climbing over the fixity table `fixities`. The
parser emits a small core:

| Source | Core |
|-|-|
| `f p1 p2 = ...` (several clauses) | `\$a0 $a1 -> case $a0 $a1 of ...` |
| guards, `where` | `Rhs` on a case alternative; zero-scrutinee case if needed |
| `a && b`, `a \|\| b` | `if` |
| `x op y` | `op x y` |
| `(op e)`, `(e op)` | `flip op e`, `op e` |
| `[a, b]`, `"ab"` | cons chains |
| `[a..b]`, `[a, b..c]` | `enumFromTo`, `enumFromThenTo` |
| `(p1, p2) = e` | `$p = e` plus one `case $p of` binding per variable |

`case` takes several scrutinees, so multi-argument clauses need no tuple.
A failed pattern, or a fallthrough past every guard, moves to the next
alternative, which gives Haskell's clause semantics.

Generated names start with `$`, which user identifiers cannot contain.

## Checking (`check.c`)

`compile_chunk` processes one module or REPL input:

1. Register data types and constructors. Constructor ids are shared with
   the runtime table `fk_cons`.
2. Add a global for each binding.
3. Split top-level, `let` and `where` binding groups into strongly
   connected components (Tarjan) in dependency order. Free-name analysis
   respects shadowing. A recursive component that contains a non-lambda is
   an error.
4. Infer each component.
5. Default remaining `Num` variables to `Int`, and move `main` last.
6. Run `resolve_top` on each binding.

On any error the chunk rolls back: name maps through an undo log, and
global, constructor and type tables by truncation. A failed REPL line
leaves no trace.

Inference:

- Types are mutable union-find variables or constructor applications.
- Each variable has a `level`. Generalization marks variables above the
  current level as `GENERIC` in place; instantiation copies them.
- Flags on variables: `num` for the `Num` constraint, `skolem` for rigid
  signature variables. A skolem's level detects escape, using the same
  check that adjusts levels during unification.
- Every integer literal records its type. Resolution later rewrites
  literals whose type is `Float` into float literals.

## Closure conversion (`resolve.c`)

Every lambda gets a flat frame: parameters in slots `0..n-1`, then one slot
per variable bound by `let`, `where` or a pattern in its body. A variable of
an enclosing lambda becomes a free variable. It is copied into the closure
at creation, and lambdas in between capture it too.

Each variable reference resolves to `LOCAL slot`, `FREE index` or
`GLOBAL index`. A reference to a binding whose right side is a lambda also
records that `Lambda` (`known`), so backends can call it directly.

Frames never outlive their call, so both backends keep them on the C stack.
Top-level values become zero-parameter lambdas (`Bind.init`), run once.

## Runtime (`rt.c`)

A value `V` is one word:

| Low bits | Meaning |
|-|-|
| `...1` | Int, 63-bit |
| `..10` | Char |
| `..00` | pointer to an object |

Objects start with an 8-byte header `{kind, n, aux}`:

| Kind | Layout |
|-|-|
| `K_CON` | `aux` = constructor id, `n` fields |
| `K_FLOAT` | one `double` |
| `K_CLOSURE` | `aux` = arity; code pointer, info pointer, `n` free variables |
| `K_PAP` | `aux` = held argument count; closure, held arguments |

Nullary constructors, constructor functions and primitive closures are
static objects outside the GC heap, so `b == fk_true` tests a Bool.

All code has the signature `V code(V self, V *args)`. `fk_apply` handles
the three call cases: exact arity calls the code, too few arguments builds
a PAP, and too many applies the result to the rest. Interpreted closures
use `interp_entry` as their code and the `Lambda` as their info, so the
runtime does not distinguish them from compiled closures.

### GC

Non-moving conservative mark-sweep:

- Objects up to 32 words live in 1 MiB aligned chunks, one object size per
  chunk. Mapping any word to its object takes a binary search over chunk
  addresses and one division, and handles interior pointers.
- Larger objects are kept in a sorted array of separate allocations.
- Roots: the funk thread's stack from the current frame to its base,
  registers spilled by `setjmp`, and registered arrays (the globals).
- Every word of a live object is scanned, so the collector needs no
  per-kind layout.
- Collection runs when allocation since the last GC exceeds
  max(32 MiB, live bytes). Sweep rebuilds the free lists.

`fk_run` starts the program on a thread with a 4 GiB `MAP_NORESERVE`
stack. That fixes the stack bounds for scanning. `FK_STACK_CHECK` turns
overflow into a runtime error.

## Interpreter (`interp.c`)

`eval(expr, frame, self, tail)` loops over `let`, `if` and `case` without
recursing. For a saturated call in tail position to an interpreted closure
whose frame fits, it writes the arguments into the current frame and
returns 0, and `run` restarts with the new body. Tail calls therefore run
in constant stack, including mutual recursion. Other saturated
interpreted calls go to `run` directly instead of through `fk_apply`.

## C generator (`cgen.c`)

Output: the embedded runtime source, a global array `G`, one
`static V fn_N(V self, V *a)` per lambda, and `program()`, which registers
constructors and initializes globals in dependency order.

- Frame slots become locals `lN`; free variables are `FK_FREE(self, i)`.
- Arguments go into temporaries in order before each call, since C leaves
  argument evaluation order unspecified.
- Saturated calls to known lambdas, primitives and constructors are direct
  C calls; everything else goes through `fk_apply`.
- A self call in tail position assigns the parameters and jumps to `top`.
- A case alternative becomes a pattern test, then bindings, then guards.
  A failure jumps to the next alternative's label.
- Constructor tag tests are skipped for single-constructor types.

`funk build` writes the C to a temporary file and runs
`$CC -O2 -o OUT FILE.c -lm -lpthread`.

## REPL (`main.c`)

Each input is a chunk. `is_decl_text` classifies it: `data`, a signature,
or `=` at bracket depth 0 before any expression keyword means a
declaration. An expression is compiled as a binding named `$it`, printed
unless it is `()`, then aliased to `it`. Compile errors and runtime errors
both return to the prompt through `longjmp`.

## Build and tests

`make` builds `tools/embed`. It inlines the local `#include`s of `rt.c` and
embeds the result, plus `lib/prelude.fk`, as C arrays in
`build/embedded.c`. The behaviour of `tests/run.sh` is documented at the
top of the script.
