# Modules and `import`: design note

Status: implemented, released in 0.6.0; see "Phase 1 result", "Phase 2 result" and "Phase 3 result". It builds on the syntax [records.md](records.md#modules) reserves.

## Problem

A funk program is one file plus the prelude. Code cannot be shared between programs except by copying it: `fixed9` is copied between `bench/game/nbody.fk` and `spectralnorm.fk`. A program cannot be split into files. Every Haskell program with `module` or `import` fails to parse ([haskell-compat.md](haskell-compat.md)).

## What exists

- **Chunks.** `compile_chunk` checks and elaborates one parsed module into a `Chunk`. A program is two chunks: the prelude, then the file (`driver.c`). Each REPL input is one more. A chunk sees every global of the chunks before it.

- **One global table.** `globals` and the name map `gmap` are flat. `global_alias` adds a second name for a global, as the REPL does for `it`. Types (`tmap`) and constructors are flat too.

- **Rollback.** A failed chunk undoes its name-map entries through an undo log and truncates the tables.

- **Whole-program backends.** `cgen_program` takes an array of chunks and emits one C file. Specialization, fusion, reuse copies and eager calls all read bodies across chunk boundaries, so the prelude's `map` is fused into a program's loop.

- **Type namespaces.** `Shape.area` is a global named `Shape.area`. Five places in `check.c` split a global name at its first `.` and test for an uppercase first letter (lines 359, 766, 1270, 1307, 2609).

- **Fixities** are session-wide: `prescan_fixities` installs a file's declarations before parsing it.

- **Reserved prelude names.** A program cannot define a name the prelude defines ([design.md](design.md#prelude-names-are-reserved)).

- **Classes** are a bitmask, `uint64_t cls`: at most 64 in a session. The prelude has 9.

Modules need no change to this structure. An imported module is one more chunk, compiled before the program that imports it. The per-instance context in TODO.md is unrelated: it allows several sessions per process, and modules run in one.

## Proposal

### Files and names

- **A module is a file.** `import geometry` reads `geometry.fk`; `import shapes.geometry` reads `shapes/geometry.fk`.

- **Search path.** The importing file's directory, then each directory given with `-M DIR`, in order. A module found by two paths is compiled once, keyed by `realpath`.

- **Names are lowercase**, as records.md reserves. Uppercase after `.` stays a type or constructor: `geometry.Shape`, `geometry.Circle`.

- **Header.** `module geometry where` is optional. If present, it must match the file's path.

- **Cycles** are an error that names the cycle. Haskell resolves them with `.hs-boot` files; funk has no use for them yet.

### Imports

| Form | Brings into scope |
|-|-|
| `import geometry` | `geometry.area`, `geometry.Shape`, `geometry.Circle` |
| `import geometry (area, Shape(..))` | the above, plus `area`, `Shape`, `Circle` unqualified |
| `import shapes.geometry as g` | `g.area`, `g.Shape` |

- **The qualifier is one identifier**: the path's last segment, or the `as` name. `shapes.geometry.area` is not a name. Allowing it would make `a.b.c` ambiguous between a module path and a chain of field reads, and the parser would need the import list to tell them apart.

- **Qualified by default.** A plain `import` adds no unqualified names. Adding a function to a module then cannot break a program that imports it, which is the problem design.md records for the prelude.

- **Unqualified names come from a list.** Two lists that bring in the same name are an error at the second `import`.

- **Prelude names stay reserved** in every module, for the same reason as today. An unqualified `map` always means the prelude's.

- **A local variable named like an imported qualifier is an error**, as records.md reserves. It keeps `geometry.area` from reading as a field of a variable `geometry`.

### Exports

- **No list exports every top-level name**, as in Haskell.

- **`module geometry (area, Shape(..)) where`** exports only those. `Shape` alone, without `(..)`, exports the type without its constructors and fields, so it is abstract.

- **Instances are always exported.** They are global and coherent: two instances of one class for one type, in any two modules of a program, are an error, as today in one file.

- **Fixities follow their operator.** An imported operator carries its module's fixity.

### Type namespaces

`Shape.area` defined in `geometry` is `geometry.Shape.area`. Method calls (`s.area`) find it through `s`'s type and need no import. The five name-splitting sites in `check.c` move to one helper that finds the type segment by its uppercase first letter.

## Implementation

- **Parser.** It parses the header and the imports first. The driver compiles the imports. The parser then pre-scans fixities and parses the body, with the imported operators' fixities installed. Each new token form gets an error if misused: `module`, `import`, `as`, and `x.Y` with no spaces, which today lexes as composition.

- **Driver.** A depth-first walk from the program compiles each module once, in dependency order. On a cycle it reports the path. Each module's chunk goes into the `cgen_program` array, after the prelude and before the program.

- **Scope.** A module's definitions are registered as `geometry.area`. While its chunk compiles, an alias `area` is installed, plus aliases for its unqualified imports. All are undone through the undo log afterwards. Other modules see only the qualified name. The prelude keeps unqualified names, so fusion still finds `map` and the other pipeline functions by name (`core_name` in `fuse.c`).

- **Rollback.** A failed import rolls back every chunk compiled for it: the driver takes one undo mark before the first. In the REPL, a failed `import` leaves no trace, as a failed line does today.

- **REPL.** `import geometry` as an input compiles the module relative to the current directory and adds its qualifier. `:load` follows the loaded file's imports.

- **Generated C** is unchanged. Workers are `w_N`, and names appear only in comments.

## Alternatives

- **Haskell's import semantics.** A plain `import M` brings every name unqualified, and `import qualified M` none. It is familiar, and ports keep their imports. Against: every name a module adds can clash in importers, and funk's lowercase module names already require editing every Haskell `import`. Rejected for the default; a Haskell import compatibility layer (below) covers ports.

- **Uppercase module names** (`import Geometry`). This matches Haskell. But `Geometry.area` and `Shape.area` would then parse the same, and only scope could tell a module from a type namespace. Haskell code often aliases a module to its type's name (`import qualified Data.Map as Map` beside the type `Map`). Under funk's type namespaces that alias would be ambiguous.

- **Separate compilation**, one C file and object per module. It would make rebuilds faster. The prelude could stay whole-program, so its pipelines would still fuse. But specialization, fusion, reuse copies and eager calls would stop at user modules' boundaries: a list function in one module would not fuse into its caller in another. Whole-program compilation is kept; a cache of checked chunks per file is a later option.

- **No modules; a textual `include`.** About 40 lines. It shares code, but with no qualification, no export control and no reserved-name protection between files.

## Haskell import compatibility (later)

Accept `module Main where` and `import Data.List (sortBy)` for the base modules whose names the prelude already defines. Such an import checks that the listed names exist and binds nothing new. `import qualified Data.Map as Map` stays an error, since funk has no `Map`. This belongs to haskell-compat.md, and is about 40 lines in the parser.

## Tests

- **Run tests,** in `tests/run/modules/`, each a main file with modules in a subdirectory the run glob skips:
  - qualified use;

  - an import list;

  - `as`;

  - a two-level path;

  - a diamond, one module imported by two others, compiled once;

  - an imported operator with its fixity;

  - an instance defined in a module;

  - an abstract type;

  - fusion across a module boundary.

- **Error tests:**
  - a cycle;

  - a missing module;

  - a name clash between two import lists;

  - a non-exported name;

  - a hidden constructor;

  - a local variable named like a qualifier;

  - a header that does not match its path;

  - a prelude name defined in a module.

- **The REPL:** `import` at the prompt, and a failed `import` that leaves no trace.

## Size

About 400 lines: parser 120, driver 100, scope and the name-splitting helper in `check.c` 150, fixities 30. Plus tests. An estimate.

## Phasing

1. Files, the search path, qualified imports and `as`; no export lists, no unqualified imports.

2. Import lists and export lists, with abstract types.

3. The REPL's `import`, then the Haskell compatibility layer.

## Decisions

- Qualified-only by default.

- A single-identifier qualifier.

- The search path is given with `-M DIR` only. A command then shows where every module comes from; `FUNK_PATH` can be added later if `-M` proves tedious.

- `fixed9` stays copied. The Benchmarks Game requires one source file per program, and `bench/game/` follows it.

## Phase 1 result

Files, the search path, qualified imports and `as`, in `src/qualify.c` (234 lines), `parse.c`, `driver.c`, `check.c` and `cli/main.c` (264 lines). Tests: `tests/run/modules.fk` with its modules in `tests/run/modules/`, nine error tests (`tests/errors/mod_*.fk`), and `tests/repl/modules.in`.

It departs from the proposal in these points:

- **Renaming instead of aliases.** The proposal installed short aliases in the name maps while a module compiled. Generated globals are keyed by name, such as reuse copies (`step@own1`) and specializations, and two modules' `step` would share them. `qualify.c` renames the parsed module instead: every top-level name of a module becomes `prefix.name` before `compile_chunk`, and later passes see unique names.

- **Qualified operators**, not in phase 1 as proposed: `a vec.<+> b`, and `(vec.<+>)`. An imported operator has its own fixity. Fixities stay session-wide, as before modules.

- **Module names are unique in a program.** Two files with one last segment, such as `dup` and `sub.dup`, are an error, since the prefix is that segment.

- **The header** must match the import path's last segment, not the whole path. A module imported from two directories has two paths.

- **`module`, `import` and `as`** are reserved only in a file's header, so programs that use them as names still compile.

- **The REPL.** `:l` follows a file's imports and compiles each module once per session. A failed load keeps the modules it compiled, as `:l` keeps a file's earlier definitions. Qualifiers are not in scope at the prompt; that is phase 3's `import`.

Programs without imports are unchanged: the 67 programs in `bench/`, `examples/` and `tests/run/` emit the same C with the type-namespace test the five `check.c` sites used before (`ns_dot` replaces it).

## Phase 2 result

Import lists, export lists and abstract types, in `qualify.c` (now 399 lines), with export tables kept per module by `driver.c`, and a check in `check.c` that an abstract type's fields stay in its module. Tests: `tests/run/modlists.fk` with its modules in `tests/run/modlists/`, and 10 more error tests.

```haskell
module stack (Stack, empty, push, pop) where   -- Stack is abstract
import modlists.shapes (Shape(..), HasName(..), (<->))
```

Rules settled while implementing:

- **A type's namespace functions go with the type.** Exporting `Stack` exports `Stack.size`, so `s.size` works outside, and none can be kept inside. An export list cannot name `Stack.size` alone.

- **Fields need `T(..)`.** `T(A, B)` names constructors only. A type exported without `(..)` is abstract: outside its module, `s.items`, `s { items = ... }`, record construction and record patterns are errors, even where a constructor is exported.

- **A type not exported at all keeps its fields and namespace functions inside**, though its values may leave through an exported function (`mkSecret :: Int -> Secret`). Dot syntax resolves by the value's type in `check.c`, not by name in `qualify.c`, so `qualify.c` records each type whose fields stay inside (`sealed_types`) and each type not exported (`hidden_types`) for `check.c` to enforce. A first version recorded only types exported without `(..)`, and `(mkSecret 7).sv`, record update, `(.sv)` and `(mkSecret 7).reveal` all worked outside; `tests/errors/mod_leak_*.fk` are those four.

- **A class exported without `(..)` keeps its methods inside**, so no other module can define an instance of it.

- **No re-exports.** An export list names only the module's own definitions.

- **A name an import list brings in** clashes with the same name from another list, and with a definition in the importing file; both are errors at the second.

A phase 1 bug surfaced here: the program's body was parsed before its imports were compiled, so an imported operator had the default fixity, `infixl 9`. Phase 1's test gave the same output at either fixity. The parser now stops after the header (`parse_begin`); the driver compiles the imports, then parses the rest (`parse_finish`). `modlists.fk` uses an `infixr 5` operator that `infixl 9` would reject.

## Phase 3 result

`import` at the REPL prompt, and Haskell's base imports, in `driver.c`, `parse.c` and `qualify.c`. Tests: `tests/run/hsimports.fk`, `tests/repl/imports.in`, and four error tests.

- **The REPL.** `import a.b [as q] [(items)]` at the prompt compiles the module, relative to the current directory or under `-M`, and adds its qualifier and listed names to the REPL's scope. `:l` adds the loaded file's imports, as GHCi puts a loaded module's scope at the prompt. A qualifier bound again is rebound. Declarations, expressions and `:t` are qualified against that scope.

- **Base imports.** A capitalized import names a Haskell base module: `Prelude`, `Data.List`, `Data.Char`, `Data.Maybe`, `Data.Either`, `Data.Bits`, `Data.Function`, `Data.Tuple`, `Data.Ord`, `Control.Monad` or `System.IO`. The prelude stands in for each. Every listed name must be in the prelude, and none is added, since the prelude's names are in scope already. `qualified` is accepted. Without `as`, the qualifier is the whole path (`Data.Char.ord`), as in Haskell; a qualified name maps to the prelude's (`C.ord` is `ord`). Another capitalized module, such as `Data.Map`, is an error that lists the eleven. `hiding` is an error.

- **`module Main (main) where`** is accepted in the program, and its export list ignored.

- **Constructor names at run time** are unqualified, as derived `Show` prints them: a value shown without a `Show` instance read `stack.MkStack {...}` in the REPL.
