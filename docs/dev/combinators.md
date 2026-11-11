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

- **Times** in cycles are `Float`s, rounded to a tick (above). A denominator that does not divide 46126080, such as 17's in `iter 17`, rounds.
- **Speed factors** are `Float`s, as times are: `fast`, `slow`, `ply`, `segment`, `hurry`, `loopAt`, `smash`, `fastGap`, `fastSqueeze` and the rest. Each is taken as its nearest ratio with a denominator up to 65536 (`toRatio`), then applied exactly in ticks, as `_fastR n d` is. Tidal reads a mini-notation decimal as a `Double` made `Rational`: `"0.6"` is 5404319552844595/2^53, not 3/5. funk takes 3/5, as Tidal does for the literal `0.6`. The `Double`'s ratio cannot be held in ticks.
- **`pureValue` is deferred.** As Tidal 1.10, a constant argument applies directly, without a join. funk is strict, so the value is held as a function of `()`; a shortcut computes it as the pattern is built, where Tidal's may wait for a query.
- **`zoom` past a cycle clips to the query.** Tidal's `zoomArc` with an end past 1 returns an onset twice and one outside the query, so `steptake 5 "a b c"` would play events twice in a host; funk's does not, as for `stripe`.
- **`substruct'`**, and so `stripe`, clips events to the query. Tidal's returns each event over its whole span, so funk's hosts, which query in blocks, would play an onset once per block that overlaps it.
