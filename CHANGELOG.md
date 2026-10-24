# Changelog

Format: [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [Unreleased]

### Added

- Language with Haskell syntax: clauses with pattern matching and guards,
  `where`, `let`, `case`, `if`, lambdas, layout rule, `data`, sections,
  ranges, operator definitions, `.`, `|>`, `>>>` and `$`.
- Hindley-Milner inference with optional, rigid signatures. Numbers use a
  single `Num a` constraint over `Int` and `Float` with polymorphic
  literals, chosen over type classes for size; see `docs/dev/design.md`.
- Strict evaluation with effects sequenced by `>>`.
- REPL (`funk`) and interpreter (`funk FILE`).
- Compiler to standalone C (`funk build`, `funk emit-c`).
- Shared runtime with a conservative mark-sweep GC and a 4 GiB lazily
  committed stack.
- Prelude of 78 functions over lists, `Maybe`, `Either`, characters and
  strings.
- Test suite (`make test`): every run test is checked under both backends.
