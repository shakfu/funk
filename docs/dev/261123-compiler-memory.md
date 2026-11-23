# Compiler memory in long sessions

Date: 2026-11-23. Review items C8 to C10 of [REVIEW.md](../../REVIEW.md). Measured on an M1, macOS, `RelWithDebInfo`.

## Method

- Inputs: one-line `pat{i%4} = p.fast "<2 3>" (p.rev (p.every 3 (p.fast 2) "0 i 4 [5 7]"))` after `import pattern as p`, fed to `funk repl -M lib`. Runs of 200 and 800 inputs; the difference over 600 is the cost of one input.
- A temporary copy of `util.c` recorded each `xmalloc`'s caller, and the live bytes per caller at exit. It is not in the tree.
- RSS includes the funk heap, which is not collected below its 128 MiB floor, so live compiler bytes are the measure.

## Results

| | Before | After |
|-|-|-|
| Compiler bytes allocated per input | 96 KB | 93 KB |
| Compiler bytes left live per input | about 92 KB (inferred: 28 KB plus the 64 KB below, never freed) | 27.9 KB |
| RSS growth per input | 115 KB | 46 KB |
| Specialization copies before the session cap | 4000 per session: reached after about 1100 inputs of `p.fmap (\x -> x + i)` | 4000 per input |

What was freed:

- `Own` environment nodes of the reuse analysis: 48 KB per input, all garbage once `reuse_cells` returns. They now come from blocks freed at its end.
- Token arrays: 16 KB per input, lexed twice (`is_decl_text`, then the parser) and never freed. The AST copies what it needs.

## What remains

- 27.9 KB per input stays live: the input's AST, types, dictionaries and specialization copies. The interpreter runs the AST, and code defined earlier keeps calling a redefined name's old version, so most of this is reachable. Reclaiming the rest needs to know which definitions nothing reaches: a collector or reference counts for compiler objects. Not planned. A 2-hour set at one input every 10 s keeps about 20 MB.
- The prelude and `pattern` leave 78.5 MB live once compiled, 24 MB of it `copy_expr` copies: `own_src` for reuse and `orig` for specialization of constrained functions. Each plugin instance pays this once.
- `charge` bills a whole `xrealloc` rather than its growth, so the compile budget overstates use. The old size is not recorded.
