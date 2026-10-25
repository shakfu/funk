# Architecture

How funk 0.1 is built. The reasons behind each choice are in
[design.md](design.md).

## Pipeline

```text
source --lex.c--> tokens (layout applied)
       --parse.c--> AST, desugared
       --check.c--> SCC groups, inferred types, class constraints
       --elab.c--> dictionaries passed explicitly, specializations
       --resolve.c--> frame slots, free-variable lists
           |--interp.c--> evaluate (REPL, funk run)
           '--cgen.c----> C source --$CC--> executable
```

Both backends link the same runtime, `rt.c`.

| File | Lines | Role |
|-|-|-|
| `src/lex.c` | 353 | tokenizer, layout rule |
| `src/parse.c` | 1431 | recursive-descent parser, desugaring |
| `src/check.c` | 1966 | data types, classes, SCC analysis, type inference, chunk driver |
| `src/elab.c` | 475 | dictionary passing, specialization |
| `src/resolve.c` | 158 | closure conversion |
| `src/interp.c` | 173 | interpreter |
| `src/cgen.c` | 321 | C generator |
| `src/rt.c`, `rt.h` | 898 | values, GC, apply, `show`, compare, primitives |
| `src/prims.def` | 64 | primitive table: C name, funk name, arity, type |
| `src/main.c` | 323 | CLI, REPL, `cc` invocation |
| `src/util.c`, `funk.h` | 549 | allocation, interning, maps with undo, AST types |
| `lib/prelude.fk` | 274 | standard functions in funk |
| `tools/embed.c` | 35 | embeds `rt.c` and the prelude into the binary |

## Lexing and layout (`lex.c`)

The tokenizer marks each token that starts a line. A second pass applies the layout rule with a context stack. Each implicit context records:

- its column;

- the bracket depth at which it opened (`)`, `]` and `,` close it);

- pending `if` count (`then` and `else` close inner blocks);

- whether it is a `let` block, plus a count of `let` blocks closed before their `in` arrived. This stops `in` from closing an enclosing block after indentation already closed the `let`.

Virtual braces (`T_VLBRACE`, `T_VRBRACE`) are distinct token kinds, so an explicit `}` cannot close a layout block. A `{` that does not follow `let`, `where` or `of` is a record brace and nests like `(`.

A `.` with no space before it and a lowercase name directly after it is one `T_FIELD` token: `p.x`, `(.x)`, `Shape.area`.

## Parsing and desugaring (`parse.c`)

Expressions use precedence climbing. Fixities come from the built-in table `fixities`, overridden by `user_fixities`. Before parsing a module, `prescan_fixities` installs every fixity declaration in it, so an operator can be used above its declaration. `main.c` undoes them with a failed REPL input. The parser emits a small core:

| Source | Core |
|-|-|
| `f p1 p2 = ...` (several clauses) | `\$a0 $a1 -> case $a0 $a1 of ...` |
| guards, `where` | `Rhs` on a case alternative; zero-scrutinee case if needed |
| `a && b`, `a \|\| b` | `if` |
| `x op y` | `op x y` |
| `(op e)`, `(e op)` | `flip op e`, `op e` |
| `[a, b]`, `"ab"` | cons chains |
| `do { p <- e; e2; let ds; e3 }` | `case e of p -> case e2 of _ -> let ds in e3` |
| `[a..b]`, `[a, b..c]` | `enumFromTo`, `enumFromThenTo` |
| `(p1, p2) = e` | `$p = e` plus one `case $p of` binding per variable |

`case` takes several scrutinees, so multi-argument clauses need no tuple. A failed pattern, or a fallthrough past every guard, moves to the next alternative, which gives Haskell's clause semantics.

Generated names start with `$`, which user identifiers cannot contain.

## Checking (`check.c`)

`compile_chunk` processes one module or REPL input:

1. Register data types and constructors. Constructor ids are shared with the runtime table `fk_cons`. `deriving` was already expanded by the parser into instance declarations.

2. Register classes and instances. A class gets a dictionary constructor `$D.C`, one selector per method, and one per superclass. Default methods become bindings `$def.C.m`. Each instance method becomes a binding `$m.C.T.m` whose given type is the method type at the instance.

3. Add a global for each binding.

4. Split top-level, `let` and `where` binding groups into strongly connected components (Tarjan) in dependency order. Free-name analysis respects shadowing. A recursive component that contains a non-lambda is an error.

5. Infer each component. Record construction and record patterns are rewritten to positional form here. Field access and record update wait until the record's type is known, then become `case` (see [records.md](records.md#resolution)); backends never see them.

6. Default remaining `Num` variables to `Int`, report other open constrained variables as ambiguous, and move `main` last.

7. Elaborate each binding (`elab.c`), then build each new instance's dictionary `$dict.C.T`. Selectors, dictionaries and specializations go to `Chunk.raw`.

8. Run `resolve_top` on each binding.

9. Warn about each `case` whose alternatives do not cover every constructor. This uses Maranget's usefulness algorithm, extended to return an unmatched pattern. An alternative counts only if one of its guards cannot fail. Int and Char literals count as infinite domains.

On any error the chunk rolls back: name maps through an undo log, and global, constructor and type tables by truncation. A failed REPL line leaves no trace.

Inference:

- Types are mutable union-find variables or constructor applications.

- Each variable has a `level`. Generalization marks variables above the current level as `GENERIC` in place; instantiation copies them.

- A variable carries `cls`, a bitmask of the classes it must be an instance of, closed under superclasses. Binding it to a constructor type looks up an instance for each class, and the instance context adds classes to the type's arguments.

- `skolem` marks rigid signature variables. A skolem's level detects escape, using the same check that adjusts levels during unification. A skolem's classes are fixed by its signature.

- Every integer literal records its type. Resolution later rewrites literals whose type is `Float` into float literals.

- Each variable reference records the instantiated constraints of its scheme (`Expr.preds`). Each binding records the constraints it generalized (`Bind.preds`), in the order `preds_of` gives. Elaboration needs both.

## Elaboration (`elab.c`)

A binding with constraints gets one dictionary parameter per constraint, prepended to its lambda. A use gets its dictionaries as leading arguments of the same application, so a call to a known function stays saturated. A dictionary is a parameter in scope, a superclass selector applied to one, or an instance dictionary, applied to its context's dictionaries if it has one. The rewrites that avoid dictionaries at known types are described in [type-classes.md](type-classes.md#performance).

Installation runs in three steps per chunk, in both backends: function closures, then dictionaries (superclasses first), then top-level values. Dictionaries refer to method functions, and values may use dictionaries.

## Closure conversion (`resolve.c`)

Every lambda gets a flat frame: parameters in slots `0..n-1`, then one slot per variable bound by `let`, `where` or a pattern in its body. A variable of an enclosing lambda becomes a free variable. It is copied into the closure at creation, and lambdas in between capture it too.

Each variable reference resolves to `LOCAL slot`, `FREE index` or `GLOBAL index`. A reference to a binding whose right side is a lambda also records that `Lambda` (`known`), so backends can call it directly.

Frames never outlive their call, so both backends keep them on the C stack. Top-level values become zero-parameter lambdas (`Bind.init`), run once.

A non-recursive local binding that is not a lambda, literal or constructor also gets a zero-parameter lambda (`Bind.thunk`). Backends allocate a `K_THUNK` in its slot, and references to it (`Expr.lazy`) call `fk_force`.

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
| `K_THUNK` | closure layout, arity 0; after forcing, code is 0, info holds the value, free variables are cleared |

Nullary constructors, constructor functions and primitive closures are static objects outside the GC heap, so `b == fk_true` tests a Bool.

All code has the signature `V code(V self, V *args)`. `fk_apply` handles the three call cases: exact arity calls the code, too few arguments builds a PAP, and too many applies the result to the rest. Interpreted closures use `interp_entry` as their code and the `Lambda` as their info, so the runtime does not distinguish them from compiled closures.

### GC

Non-moving conservative mark-sweep:

- Objects up to 32 words live in 1 MiB aligned chunks, one object size per chunk. Mapping any word to its object takes a binary search over chunk addresses and one division, and handles interior pointers.

- Larger objects are kept in a sorted array of separate allocations.

- Roots: the funk thread's stack from the current frame to its base, registers spilled by `setjmp`, and registered arrays (the globals).

- Every word of a live object is scanned, so the collector needs no per-kind layout.

- Collection runs when allocation since the last GC exceeds max(32 MiB, live bytes). Sweep rebuilds the free lists.

`fk_run` starts the program on a thread with a 4 GiB `MAP_NORESERVE` stack. That fixes the stack bounds for scanning. `FK_STACK_CHECK` turns overflow into a runtime error.

## Interpreter (`interp.c`)

`eval(expr, frame, self, tail)` loops over `let`, `if` and `case` without recursing. For a saturated call in tail position to an interpreted closure whose frame fits, it writes the arguments into the current frame and returns 0, and `run` restarts with the new body. Tail calls therefore run in constant stack, including mutual recursion. Other saturated interpreted calls go to `run` directly instead of through `fk_apply`.

## C generator (`cgen.c`)

Output: the embedded runtime source, a global array `G`, one `static V fn_N(V self, V *a)` per lambda, and `program()`, which registers constructors and initializes globals in dependency order.

- Frame slots become locals `lN`; free variables are `FK_FREE(self, i)`.

- Arguments go into temporaries in order before each call, since C leaves argument evaluation order unspecified.

- Saturated calls to known lambdas, primitives and constructors are direct C calls; everything else goes through `fk_apply`.

- A self call in tail position assigns the parameters and jumps to `top`.

- A case alternative becomes a pattern test, then bindings, then guards. A failure jumps to the next alternative's label.

- Constructor tag tests are skipped for single-constructor types.

`funk build` writes the C to a temporary file and runs `$CC -O2 -o OUT FILE.c -lm -lpthread`.

## REPL (`main.c`)

Each input is a chunk. `is_decl_text` classifies it: `data`, a signature, or `=` at bracket depth 0 before any expression keyword means a declaration. An expression is compiled as a binding named `$it`, printed unless it is `()`, then aliased to `it`. Compile errors and runtime errors both return to the prompt through `longjmp`.

## Build and tests

`make` builds `tools/embed`. It inlines the local `#include`s of `rt.c` and embeds the result, plus `lib/prelude.fk`, as C arrays in `build/embedded.c`. The behaviour of `tests/run.sh` is documented at the top of the script.
