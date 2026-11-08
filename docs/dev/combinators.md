# Combinators

Tidal's transformations in `lib/pattern.fk`, for roadmap 0.11 ([roadmap.md](roadmap.md#011----tidal-expressive-patterns)). Source: tidal-core 1.10.2 from Hackage, `src/Sound/Tidal/`: `Core.hs`, `UI.hs` and `Control.hs`, about 540 top-level definitions. Each port follows the source, not Tidal's documentation. The `main` branch of [Tidal](https://github.com/tidalcycles/Tidal) at `fefb1c3` is older than the release: it lacks `chunkInto`, `ribbon` and `into`, and its `runMarkov` differs.

`lib/tidal` (`tests/tidal/compare.sh`) runs 178 cases through Tidal and through funk and diffs the output: each case's onsets over its first cycles, in ticks, with values to 1e-9. It runs where `ghc` has the `tidal` package, and is skipped elsewhere.

## Scope

Four groups, all in scope (decided 2026-11-08):

1. **Discrete:** transformations of events that have a whole, such as `every`, `whenmod`, `~>`, `iter`, `ply`, `chunk`, `off`, `jux`, `sometimesBy`, `linger`, `struct`, `mask`, `arp`, `rolled`, `stut`.
2. **Continuous:** `sine`, `saw`, `tri`, `square`, `rand`, `irand`, `perlin`, `envL` and the like, and `segment`, `range` and `#` over them.
3. **Sample slicing:** `chop`, `striate`, `slice`, `splice`, `randslice`, `loopAt`, `hurry`. These need `d.kit` to play `begin`, `end` and `cut`.
4. **Runtime state:** `cF`, `cN`, `in0`..`in127`, `trigger`, `qtrigger`, `reset`. These need state the host owns: control input, and the cycle an evaluation started.

Order: 2, then 1, since `degradeBy`, `sometimesBy`, `segment` and `range` take continuous patterns; then 3 and 4, each with its host work.

## Continuous events

`Event`'s whole becomes `Maybe Span`, as Tidal's `whole :: Maybe Arc`. An event with no whole is continuous: it has a value over its part and never an onset, so the hosts never play it. `sig f` answers a query `q` with one such event, valued `f` at `q`'s start, as Tidal's `sig` (`Core.hs:30`).

- Where Tidal reads `wholeOrPart`, funk does too: `apLeft` and `apRight` query the other pattern over it, so `s "bd*4" # pan sine` takes 4 values.
- `apP`'s whole is the intersection where both have one, and none otherwise, as Tidal's `applyPatToPatBoth`.

The alternative was a separate `Signal a` type, sampled where a pattern needs values. It keeps `Event` as it is, but every function that takes a pattern would need a second form for a signal, or a class over both, and Tidal's `fast 2 sine` and `0.25 ~> rand` would not port as written.

Cost: a `Just` per event. To measure with the patch port's query timings (roadmap acceptance test).

## Time arguments

Tidal's times are `Rational` cycles. A funk combinator that takes a time takes a `Pattern Float` of cycles, rounded to the nearest tick, so `0.25 ~> p` ports as written: `FromFloat (Pattern a)` makes the literal an atom. A fraction whose denominator divides 720720 is exact, as `1/3`, `0.25` and `0.2` are. `shift` keeps its `Pattern Int` of ticks.

## Status

All four groups are done (2026-11-08): `d.kit` plays `begin`, `end`, `cut` and the rest ([delivery.md](delivery.md)), and `cF` and the `trigger` family read controls from OSC, MIDI CC and the plugin's parameters ([controls-in.md](controls-in.md)). Pattern.hs's `reset`, `restart` and `trigJoin` too. All 178 cases of `lib/tidal` match. Not ported: `|>`, which is `#`, as the prelude's `x |> f` takes the name.

Not ported, as Tidal's source has them inside block comments: `fill`, `pequal`, `preplace` and its kind (`prep`, `prr`, `prrw`, `prw`, `protate`, `<~>`), `scrumple`, `toMIDI`, `striateL`, `striateL'` and `en`.

## Differences from Tidal

- **Times** in cycles are `Float`s, rounded to a tick (above). A denominator that does not divide 720720, such as 17's in `iter 17`, rounds.
- **Speed factors** are `Int`s, as `fast`'s are ([patterns.md](patterns.md#implementation)): `ply`, `segment`, `hurry`, `loopAt`, `smash`, `fastGap`, `fastSqueeze`.
- **No `pureValue`.** Tidal 1.10 applies a constant argument directly, where funk joins with an `atom`. An event that crosses a cycle boundary then comes in two parts; its onset is the same.
- **No steps.** Tidal 1.10's step counts (`setSteps`, `keepSteps`) and `Stepwise.hs` are not ported.
- **`rot`** orders a cycle's events by whole, then part. Tidal's order puts each event's source position first.
- **`substruct'`**, and so `stripe`, clips events to the query. Tidal's returns each event over its whole span, so funk's hosts, which query in blocks, would play an onset once per block that overlaps it.
- **`euclid`** takes Tidal's arguments, patterns of `k` and `n` and a pattern to play: `p.euclid 3 8 36` still works, as `36` is an atom.
