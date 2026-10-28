# Patterns: design note

Status: open questions, 2026-10-28. No code yet. Context: [261027-lang-review.md](261027-lang-review.md).

funk gets its own pattern engine. Tidal ([Tidal](https://github.com/tidalcycles/Tidal)) and Strudel are reference designs, not specifications. A pattern here means a description of events over musical time, such as notes or control changes, that a program can combine and transform.

## What this changes in the review

| Review claim | Now |
|-|-|
| Rational time is required | A choice; see "Time" |
| `IsString` / `OverloadedStrings` is required | A choice; see "Literals" |
| `Monad` is required for Tidal's joins | Joins are plain functions on one `Pattern` type: `Pattern (Pattern a) -> Pattern a` |
| Higher-kinded classes serve the niche | Only if the design has several pattern-like types. With one `Pattern`, monomorphic `fmapP` and similar functions suffice. The friction log decides. |
| Haskell syntax helps because Tidal users know it | Weaker, still holds: familiarity, and combinator code is terse in this syntax |
| Pattern language, DSP layer, or both | Both: patterns on funk's thread, DSP through allocation-free exports ([realtime.md](realtime.md), R4) |

## Time

| | Rational | Integer ticks per cycle |
|-|-|-|
| Exact under | any nesting of subdivisions | divisions by factors of the resolution |
| Arithmetic | normalize per operation; boxed | `Int` only; no allocation |
| MIDI | convert | direct, as clock ticks |
| funk today | no `Fractional`; unclear whether user `Num` instances can supply `/` | works now |

Recommendation: ticks. 720720 = lcm(1..16) = 2^4 * 3^2 * 5 * 7 * 11 * 13 gives exact tuplets up to 16. Nesting beyond those factors rounds: triplets three deep need 27, which does not divide 720720. This rests on judgment, not measurement; the friction log tests it.

## Representation

- **Query function** `span -> [event]`, as in Tidal. Composes well. Allocates a list per query.
- **Event arrays per cycle.** Allocation-free, so an export can read them. Harder to compose.

Recommendation: a query function on funk's thread. It writes timestamped events into a ring that the audio thread reads (R3's ring, `Array`). Patterns evaluate at control rate, which the collector's pauses under 0.5 ms allow. Sample accuracy comes from the timestamps, not from evaluating patterns on the audio thread.

## Literals

Plain combinators first, e.g. `seq [bd, sn]`. Mini-notation, a compact string syntax such as `"bd*2 sn"`, comes only if the friction log shows combinators are too verbose for live use. Overloaded string literals would change every literal's type defaulting, so an explicit parse call such as `p "bd sn"` comes before them.

## Open

- Whether `do` over patterns is wanted. funk's `do` sequences effects (`->>`, [typed-effects.md](typed-effects.md)); a monadic `do` would conflict with that design.
- How reload replaces a running pattern: at the next cycle boundary, or at once.
- Event payload: a fixed record (note, velocity, channel) or open parameters, as in Tidal's control maps.

## Friction program

A minimal pattern engine of this design, in `psnd`. Pass:

- reload of a 200-line pattern file under 100 ms, with no glitch in running output;
- a 3:5:7 polyrhythm exact across 1,000 cycles;
- no allocation on the audio thread.
