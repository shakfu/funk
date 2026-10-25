# Roc: comparison note

Status: research, 2026-10-25. It compares funk with [Roc](https://github.com/roc-lang/roc), the closest relative funk has: a strict, pure, functional language compiled to native code, built around performance. It records what funk could adopt. Three of Roc's choices bear on open funk questions: effects ([typed-effects.md](typed-effects.md)), memory for real-time use ([ffi.md](ffi.md#pauses), [memory.md](memory.md)), and polymorphism ([type-classes.md](type-classes.md)).

## Where they stand

Roc has no release yet. Its compiler was rewritten from Rust to Zig, about 300,000 lines over 487 days. The rewrite reached feature parity with the old compiler in July 2026, and 0.1.0 is planned for later that year ([Developers Digest](https://www.developersdigest.tech/blog/roc-rust-to-zig-rewrite-feldman), summarizing Richard Feldman's post; [Weekly Rust](https://weeklyrust.substack.com/p/why-roc-is-moving-away-from-rust)). funk is 0.2.0, and much smaller.

## Comparison

| | Roc | funk |
|-|-|-|
| Evaluation | strict | strict, with lazy `let` and `where` |
| Syntax | its own: `f(x, y)`, `x.method()`, `\|a\| ...` lambdas ([tutorial](https://raw.githubusercontent.com/roc-lang/roc/main/docs/mini-tutorial-new-compiler.md)) | Haskell's |
| Effects | in the function arrow: `->` pure, `=>` effectful, with `!` names by convention. No `IO` wrapper: effectful calls are direct ([functions](https://roc-lang.org/docs/main/langref/functions/)) | untyped; decision pending |
| Source of effects | only the platform, a host written in another language: "Roc's standard library does not include any effectful functions" ([tutorial](https://raw.githubusercontent.com/roc-lang/roc/main/docs/mini-tutorial-new-compiler.md)) | the runtime's primitives |
| Ad hoc polymorphism | static dispatch only: methods, constrained by `where [a.to_str : a -> Str]`; no dynamic dispatch and no runtime overhead ([static dispatch](https://roc-lang.org/docs/main/langref/static-dispatch/)) | type classes: dictionaries, specialized away when types are known |
| Higher-kinded types | none found in the docs read; no `Monad` | none yet; B2 would add them |
| Memory | reference counting, with in-place mutation when a value's count is 1 ([Utrecht thesis](https://studenttheses.uu.nl/handle/20.500.12932/44634); [PureScript discussion](https://discourse.purescript.org/t/reference-counting-to-mutate-values-in-place-without-losing-referential-transparency/3804)) | tracing mark-sweep, with static reuse of list cells and fusion |
| Data | structural records and tag unions; nominal types optional | nominal `data`; records with type-scoped fields |
| Errors | `Try` with `Ok` and `Err`; `?` returns early on `Err` | `Maybe` and `Either`, without sugar |
| Local mutation | `var $x` and `for` loops, within the declaring function | none |
| Numbers | integers from 8 to 128 bits, `F32`, `F64`, and `Dec` as the default; explicit `_wrap`, `_saturated` and `_try` operations | `Int`, 64 bits, wrapping; `Float` |
| Tests and debugging | inline `expect`, skipped in optimized builds; `dbg`, allowed in pure code | external test files; `trace` proposed |

## Lessons

### 1. Effects in the arrow

Roc makes types tell the truth about effects without a monad. This is a fifth option for [typed-effects.md](typed-effects.md), between A'' and B1:

- **No runtime cost.** An effectful call is a direct call: A'''s 0%, against B1's 5% for the runner.
- **None of B1's traps in a strict language.** There is no `>>` special form, no `bindIO` desugaring and no strict construction of actions. Strict left-to-right evaluation orders effects, as in funk today.
- **Purity for the optimizer.** The arrow says whether a function value is pure, so fusion and eager `let` can see through closures and parameters. That is where A'''s inference gives up.
- **The cost is coloring.** Higher-order functions need effectful variants. Roc's builtins have `List.for_each!` and `map_try!`, as B1 has `mapM_`.
- **Migration.** Haskell's `IO` code would not port. funk's current programs would, with little change: effectful functions get `=>`.
- **A rule Roc does not need.** Lazy `let` and `where` bindings must not hold effectful expressions, or must be evaluated where they are bound when they do. Roc has no laziness.

For funk's strict semantics, this may fit better than A'' or B1. It is option E in [typed-effects.md](typed-effects.md#e-in-detail), designed for funk there: spelled `~>`, with effect polymorphism, and compared with the other options.

### 2. Reference counting, for pauses rather than throughput

funk's Perceus prototype was slower than tracing ([memory.md](memory.md#perceus-prototyped-by-hand)), because freeing scattered later allocations from the free lists. Roc's model never stops the program for a collection. For libremidi, where funk's collector pauses 6-15 ms against MIDI timing of about 1 ms, latency matters more than the benchmark mean. Roc's real-time behaviour is inferred here from its memory model; its docs do not claim it. Reference counting, or a collector with bounded pauses, should be re-evaluated against latency.

### 3. No higher-kinded types, and dispatch resolved at compile time

Roc does without `Functor` and `Monad`. That supports deferring B2: a strict language built for performance can work without them. Roc also guarantees there is no runtime dispatch. funk specializes when it can, but falls back to dictionaries silently. A warning, or a flag, for code that keeps dictionaries at run time would make that cost visible.

### 4. Special syntax instead of abstraction

Roc's `?` gives `Either`-style early return without a monad, as its arrows give effects without one. For funk, a `?` on `Maybe` and `Either` within a function body would give most of what `do` over `Maybe` gives, without phase 2 of type classes.

### 5. Smaller items

- **Inline `expect` tests,** run by the test command and skipped in optimized builds. They are cheap, and keep tests next to code.
- **Pure top-level constants evaluated at compile time,** which Roc does because their values cannot differ at run time. funk evaluates them at startup.
- **One entry point for effects.** All effects enter through the platform, and the standard library stays pure. For [ffi.md](ffi.md), this means foreign imports, plus the runtime's own I/O, are the only sources of effects.

### Not worth adopting

- **Roc's syntax.** funk's identity is Haskell syntax.
- **Structural records and tag unions.** They need row polymorphism, as large a change as type classes.
- **`var` and `for` loops.** They fit Roc's syntax, not Haskell's.

## Caveats

- **Sources.** The full language reference could not be read; its pages are mostly navigation over builtins. The purity rules come from the functions page and the new-compiler tutorial. The memory model comes from a thesis and a forum discussion, which may describe the old compiler.
- **Effect polymorphism.** Whether Roc can write one higher-order function for both pure and effectful arguments was not confirmed. The `!` variants in its builtins suggest it cannot.
