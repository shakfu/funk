# tree-sitter-funk

A tree-sitter grammar for funk, for highlighting in Helix and Neovim. Forked from [tree-sitter-haskell](https://github.com/tree-sitter/tree-sitter-haskell) 0.24.1 (97288e5), MIT; `LICENSE` is upstream's.

Upstream parsed 166 of funk's 239 files. Its layout scanner handled funk's layout everywhere; the failures were syntax funk adds. The fork adds:

- module names that start lowercase, in `module`, `import` and `as`: `module graph where`, `import modules.listed`;
- lowercase qualifiers on constructors, types and operators: `g.Node`, `vec.Vec`, `vec.<+>`, `stack.Stack.size`. `g.play` stays a `projection`, as funk reads `.x` after a name; whether `g` is a module is known only from the imports;
- definitions in a type's namespace: `Shape.area (Circle _ r) = ...`, `Event.onset :: ...`;
- `foreign library "m"` and `foreign struct ["header.h ctype"] T { field ["c.member"] :: type }`;
- `->>`, a function arrow, in the scanner and the grammar.

`queries/highlights.scm` drops upstream's `(variable) @type`, as Helix's copy does: where a later pattern wins, it made every variable a type. The CLAP plugin's editor colours with this query too ([docs/dev/plugin.md](../../docs/dev/plugin.md#syntax-colours)).

Changes to upstream's files carry a `funk:` comment. The scanner's are small: `->>` as an arrow, and an operator qualified by a lowercase module.

## Build and check

```sh
make            # src/parser.c from grammar.js and grammar/: tree-sitter CLI and node
make check      # every .fk file in the repository parses without ERROR or MISSING nodes
make helix      # helix/queries from queries/, with Helix's capture names
```

`make check` skips `tests/errors/layout.fk`, whose layout error is its test. `src/parser.c` is generated, 20 MB, and not committed.

## Helix

```sh
tree-sitter build -o ~/.config/helix/runtime/grammars/funk.so .
mkdir -p ~/.config/helix/runtime/queries/funk
cp helix/queries/*.scm ~/.config/helix/runtime/queries/funk/
```

In `~/.config/helix/languages.toml`:

```toml
[[language]]
name = "funk"
scope = "source.funk"
file-types = ["fk"]
comment-token = "--"
block-comment-tokens = { start = "{-", end = "-}" }
indent = { tab-width = 2, unit = "  " }
grammar = "funk"
```

## Neovim

With nvim-treesitter's `master` branch, after `require('nvim-treesitter.configs').setup`:

```lua
vim.filetype.add({ extension = { fk = 'funk' } })
require('nvim-treesitter.parsers').get_parser_configs().funk = {
  install_info = { url = '/path/to/funk/projects/tree-sitter-funk', files = { 'src/parser.c', 'src/scanner.c' } },
  filetype = 'funk',
}
```

Then `make` here, `:TSInstall funk`, and link the queries: `ln -s /path/to/funk/projects/tree-sitter-funk/queries ~/.config/nvim/queries/funk`.
