# 0.11 language changes against Tidal

Status: analysis, 2026-11-02, for the 0.11 scope in [roadmap.md](roadmap.md). Each proposal below is undecided.

Evidence:

- A grep of `tidal-core/src` on [Tidal's `main`](https://github.com/tidalcycles/Tidal), downloaded 2026-11-02: 15 files, 12,994 lines. Counts are approximate.
- Small test programs run through funk 0.9.0.

The counts measure Tidal's library. User patches were not sampled. [patterns.md](patterns.md) calls Tidal a reference and not a specification, so these counts show what Tidal's implementers needed, not what funk needs. The friction log from 0.10's acceptance test should overrule a proposal where they disagree.

## Summary

| 0.11 item | funk 0.9.0 | Use in Tidal | Proposal |
|-|-|-|-|
| `type` synonyms | rejected | 8 | do |
| `newtype` | rejected | 1 | skip for now |
| As-patterns | rejected | about 39 lines | do |
| List comprehensions | rejected | 6 | wait for friction |
| Kinds, `Functor`, `Applicative`, `Monad` | rejected | many names, little abstraction | defer |
| `Foldable` | rejected | no `Pattern` instance | drop |
| Existentials | absent | none found | drop |

## Haskell compatibility gaps

Size estimates here and below come from [haskell-compat.md](haskell-compat.md), a survey of funk 0.2.

- **`type` synonyms.** Tidal has `type Time = Rational`, `type ControlPattern = Pattern ValueMap` and `type ValueMap = Map.Map String Value`. Estimate: 60 lines. Synonyms also shorten patch signatures. Proposal: do.
- **`newtype`.** Tidal has 1. Estimate: 30 lines. Plain `data` already covers it, at a possible run-time cost. Proposal: skip for now.
- **`Rational` (67 uses) and `Fractional`.** [patterns.md](patterns.md#time) chose integer ticks, which keep time arithmetic free of allocation. Proposal: skip.

Already working in funk: `#`, `|+|` and `<~` parse as user operators, and a `Num (P a)` instance runs. Funk's `Num` differs from Haskell's: it has `fromInt` and no `signum`.

## Kinds and higher-kinded classes

Use in Tidal:

- **Raw counts:** `<$>` 377, `fmap` 92, `<*>` 41, `>>=` 20. These overstate `Pattern` use: uses on parsers, lists and `Maybe` were not separated out.
- **Monomorphic core:** these are plain functions on `Pattern` and need no kinds:
  - `<*` and `*>` ([`Pattern.hs:134`](https://github.com/tidalcycles/Tidal/blob/main/tidal-core/src/Sound/Tidal/Pattern.hs#L134)), which hide the Prelude's versions
  - `innerJoin` (`:244`), `squeezeJoin` (`:279`), `unwrap` (`:227`)
- **Generic code:** few signatures are constrained over `f`: at least 9 `|op|` operators in `Core.hs`, plus `spread'` and `samples`. The search stopped at 12 matches, so there may be more.
- **`Foldable`:** `Pattern.hs` has no `Foldable` instance for `Pattern`.

Work in funk:

- **Kind inference:** `TyDef` records only an arity, so data declarations need kind inference first ([type-classes.md](type-classes.md#later-phases)).
- **Effects:** `check.c` rejects effectful calls in a method declared with `->`. So `fmap print xs` would probably fail where `map` works. This is inferred from the error text. It is the main design risk.
- **Speed:** fusion matches prelude names, so `fmap` on lists could lose fusion unless resolved to `map` first.
- **`do`:** a `Monad` class would not enable `do` over patterns, because funk's `do` sequences effects ([patterns.md](patterns.md#open)).

Proposal: defer. Add monomorphic `apP`, `joinP` and similar functions to `lib/pattern.fk` first; it has only `mapP` today. Add classes only when a second pattern-like type exists. Names like `mapP` will look unfamiliar to Tidal users. Ad hoc overloading of `<$>` at `Pattern` alone is an alternative, and not the proposed first step.

## Existential types

Tidal does not appear to need them:

- **`forall`:** both matches are rewrite rules (`Utils.hs:151`, `:155`).
- **GADT syntax:** `data TPat a` in `ParseBP.hs:114` uses it. The 6 constructors read are ordinary; the rest were not read.
- **Mixed values:** Tidal uses a closed sum, `data Value = VS | VF | VN | ...`, in a map. Funk can already write that.

Records of closures cover dynamic dispatch ([type-classes.md](type-classes.md#later-phases)). Proposal: remove from 0.11.

## As-patterns

- **Use:** about 39 lines: `Pattern.hs` 16, `Stepwise.hs` 9, `UI.hs` 8. Example: `pace target p@(Pattern _ (Just t) _)`.
- **Work:** the parser, a pattern node, binding in both backends, and exhaustiveness checks. Under 100 lines is a guess, not a measurement.
- **Risk:** an alias keeps the matched cell live, so the reuse analysis must not reuse it. This needs a test.

Proposal: do. The change is small and depends on no other item here.

## List comprehensions

- **Use:** 6 in 12,994 lines (`Utils.hs:67`, `Pattern.hs:1280`, `:1310`, `UI.hs:1437`, `:1568`, `:1596`).
- **Work:** estimate 80 lines, desugared to `concatMap` and `filter`.
- **Risk:** [haskell-compat.md](haskell-compat.md) says fusion removes the lists, but [TODO.md](../../TODO.md) lists nested `concatMap` as unfused. Comprehensions with several generators would therefore allocate.

They might read well in live patches, such as `fastcat [atom (i * 2) | i <- [0 .. 7]]`; there is no evidence either way. Proposal: wait for a friction log entry.

## What 0.11 leaves out

Tidal's most distinctive features are not on the 0.11 list:

- **Patterned arguments.** Tidal accepts `fast "2 4"` through `patternify`, which is `innerJoin` plus `<$>` (`Pattern.hs:896`). `innerJoin` appears 229 times. About 39 lines are outside `Params.hs`, so the rest are probably there (inferred). Funk's `fast` takes an `Int`. This needs only library work.
- **Mini-notation.** `IsString` instances (`ParseBP.hs:392`) with `OverloadedStrings` make strings into patterns. [patterns.md](patterns.md#literals) prefers an explicit `p "bd sn"`, which needs no language change.
- **Control maps and `#`.** `ValueMap` and `Unionable`. Event payload is an open question in [patterns.md](patterns.md#open).

Proposed framing: call 0.11 "pattern parity", with library work first and language changes only where the library needs them. On this evidence, the language work shrinks to `type` synonyms and as-patterns.

## Open question

Should funk patterns feel familiar to Tidal users, or only draw on Tidal's ideas? This note assumes ideas only, as [patterns.md](patterns.md) does. If familiarity matters, the `Functor` and `Applicative` names and mini-notation move up, and the case for higher-kinded classes gets stronger.

## Sources

- [Tidal repository](https://github.com/tidalcycles/Tidal)
- [tidal-core `Sound.Tidal.Pattern` docs](https://hackage-content.haskell.org/package/tidal-core-1.10.1/docs/Sound-Tidal-Pattern.html)
- [Waldmann, "Time-Scaled Streams"](https://imweb.imn.htwk-leipzig.de/~waldmann/etc/untutorial/tc/monad)
- [TidalCycles forum on Functor, Applicative and Monad](https://club.tidalcycles.org/t/functor-applicative-and-monad-pattern/1233)
