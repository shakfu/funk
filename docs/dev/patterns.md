# Patterns: design note

Status: implemented, 2026-10-28: `lib/pattern.fk`, tested by `tests/lib/patterns.fk`, `tests/ffi/sched.fk` and `bench/reload.c`. All three thresholds pass outside `psnd`; `psnd` integration not started. Context: [261027-lang-review.md](261027-lang-review.md).

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
- Event payload: a fixed record (note, velocity, channel) or open parameters, as in Tidal's control maps.

## Friction program

A minimal pattern engine of this design, in `psnd`. Pass:

- reload of a 200-line pattern file under 100 ms, with no glitch in running output;
- a 3:5:7 polyrhythm exact across 1,000 cycles;
- no allocation on the audio thread.

Status:

- **3:5:7: passes.** 15,000 onsets over 1,000 cycles, each on its exact tick. Onsets queried in blocks of 7,919 ticks equal one query's, as a set, for a stack that mixes `fast 17` (rounding), `slow 3` and `shift`.
- **No allocation on the audio thread: passes.** The export in `tests/ffi/sched.fk` compiles, so the R4 check ([realtime.md](realtime.md)) has proved it allocation-free, including its calls into `pattern`.
- **Reload under 100 ms, no glitch: passes, outside `psnd`.** `bench/reload.c` (test `lib/reload`) plays 1024-sample windows at 48 kHz (21.3 ms) through `funk_call`. Loading a 200-line file and redefining `step` takes 2.0 ms; a file with a type error is rejected in 0.1 ms and the old pattern plays on. The new pattern sounds from the first window after the reload; every onset lies in its window, in order. The slowest window takes 0.33 ms, collection steps included. Over 160,000 windows (57 min), memory levels off at the collector's 128 MiB threshold. Apple M1, interpreter.

## Implementation

`lib/pattern.fk`, imported with `-M lib`. Tidal's query model: `query` returns the fragments overlapping a span, each with its event's `whole` and its `part` in the span; an event is an onset where the two start together. Combinators: `silence`, `atom`, `mapP`, `stack`, `cat`, `fastcat`, `fast`, `slow`, `shift`, `euclid`.

- `fast k` rounds times down when they do not divide by `k`. A part cut at a query's start rounds to that start, above its whole's start, so block boundaries never add or lose an onset.
- `euclid k n` puts an onset on step `i` where `mod (i * k) n < k`: maximally even, starting on step 0. Its rotations can differ from Bjorklund's algorithm, which Tidal uses.
- **Scheduling.** Tempo is an `Int`, samples per cycle, so a tick maps to a sample by an exact floor, and adjacent sample blocks partition the onsets. `sampleOnsets` returns a block's onsets in time order. `pushOnsets` puts them on a `Ring Int`, one `Int` per event (sample * 65536 + value), so a push cannot tear an event. Tempo changes are not handled.
- **The test transport stalls on underrun.** In `tests/ffi/sched.fk`, musical time advances only once funk has scheduled the block. A slow funk thread then costs silence, not a late event, and the output does not depend on timing. A live player may prefer to keep time and drop late events; that is the player's choice, not the library's.
- **Reload.** A definition binds the names it uses when it is compiled, so a `step` defined before a reload keeps the old `pat`. The host redefines `step` after each load: one compiled chunk per reload. Per window it calls `step buf w` through `funk_call`, which compiles nothing, and `writeOnsets` fills `buf`, a C array the host owns. The new pattern takes over at the next window, not at a cycle boundary: queries use absolute time, so the switch cannot shift the beat.
- Names avoid `pure`, `fmap` and `seq`, which a prelude with `Functor` and `Applicative` would reserve.

## Friction log

| # | Friction | Workaround | Fix candidate |
|-|-|-|-|
| 1 | `mapP :: (a -> b) -> Pattern a -> Pattern b` is rejected: the signature's arrows share an effect variable, and `Pattern`'s field needs `f` pure ([typed-effects.md](typed-effects.md), "A pure argument, regardless"). The message, "functions whose effects differ", names neither. | no signature | a pure-argument marker; or name the field and the parameter in the message |
| 2 | `map p.atom "abc"` fails: `String` is packed | `unpack` | the targeted error from the review |
| 3 | A field read on a parameter, `e.whole.start`, needs a signature: "cannot infer the record type" | a signature | infer from the field name when one type has it |
| 4 | A test file `pattern.fk` that imports `pattern` reports "import cycle: pattern -> pattern": the importer's directory is searched first and finds the file itself | rename | say "imports itself" || 5 | A definition keeps the names it was compiled with: after `:l`, an earlier `step` still plays the old `pat` | redefine `step` after each load | none needed; document for live use |
| 6 | Every `funk_eval` input stays compiled: about 16 KB each, and the slowest call grew from 0.12 to 0.62 ms over 40,000 calls. A host cannot evaluate per window | `funk_call` | rolling back an expression's chunk after it runs |

