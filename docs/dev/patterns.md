# Patterns: design note

Status: implemented, 2026-10-28: `lib/pattern.fk`, tested by `tests/lib/patterns.fk`, `tests/ffi/sched.fk` and `bench/reload.c`. All three thresholds pass outside `psnd`. `funk-midi` (`projects/funk-midi/main.c`) plays patterns to a MIDI port, live. The CLAP plugin is in `midi-langs` (`projects/funk-midi`); `psnd` not started. Context: [261027-lang-review.md](261027-lang-review.md).

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

- **Reload.** A definition binds the names it uses when it is compiled, so a `step` defined before a reload keeps the old `pat`. The host redefines `step` after each load: one compiled chunk per reload. Per window it calls `step buf w` through `funk_call`, which compiles nothing, and `writeEvents` fills `buf`, a C array the host owns. The new pattern takes over at the next window, not at a cycle boundary: queries use absolute time, so the switch cannot shift the beat.

- Names avoid `pure`, `fmap` and `seq`, which a prelude with `Functor` and `Applicative` would reserve.

## funk-midi

`projects/funk-midi/main.c`, the first host. A REPL over `libfunk.h` with `pattern as p` and `midi as m` imported. Input that defines `pat`, a `Pattern Int`, or `:l FILE`, recompiles `step`, and `pat` plays from the next window. `:cycle MS` changes the tempo, `:stop` and `:play` silence and resume, `:wait MS` plays for a time in a script.

- **Values.** `lib/midi.fk`: note + 128 * velocity + 16384 * (channel - 1). A plain note number plays at velocity 100 on channel 1. `m.vel` and `m.chan` clamp their arguments, so a typo cannot stop playback.

- **Note-offs** come from each event's whole: `sampleEvents` returns its duration. At one time, note-offs go before note-ons, so a note that ends where the next starts is released first.

- **Threads.** funk's thread schedules 10 ms windows 50 ms ahead and handles input. A sender thread sends each message at its time from a heap, so input and reloads never delay a message. A new `pat` misses the onsets already inside the lookahead: a redefinition at a cycle's start sounds from the next cycle.

- **One clock** from start-up, so patterns keep one cycle grid across redefinitions, `:stop` and `:play`. A tempo change takes effect at the next unscheduled window, from the tick reached there.

- **Errors.** A definition with a type or runtime error leaves the playing pattern; a pattern that fails when queried stops until `pat` is defined again.

- **Timing, macOS, M1.** As a default-policy thread, the sender woke from a timed wait up to 8 ms late (median 2 ms). Waking 2 ms early and sleeping the rest did not help, nor did the user-interactive QoS class. As a time-constraint thread (`THREAD_TIME_CONSTRAINT_POLICY`): median 0.06 ms, max 0.32 ms over 600 messages. Through CoreMIDI to a second process, notes a cycle apart arrived within 0.03 ms of each other. Linux is unmeasured.

- **Build.** Without libremidi, `funk-midi` can only print (`--print`, `--no-sleep`), which `tests/hosts/midi.txt` uses. `-DFUNK_LIBREMIDI=ON` adds MIDI output, with the vendored `thirdparty/libremidi`. `--audio` plays `sig` too ([audio.md](audio.md), item 5); the vendored `thirdparty/miniaudio` gives the audio device, unless `-DFUNK_MINIAUDIO=OFF`.

## CLAP plugin

`projects/funk-clap/funk_clap.cpp`, an adapter for `projects/clap`, a copy of `midi-langs`' CLAP framework ([plugin.md](plugin.md#decision)). One instance per process, which the framework already enforces.

- **Threads.** libfunk's calls run on funk's thread; the adapter starts one session with the first instance, keeps it until the plugin is unloaded, and queues each call to it from the framework's evaluator thread. While a call waits, a Stop calls `funk_interrupt`.

- **Modes.** In Live mode `pat` loops in 10 ms windows; an expression of type `Pattern Int` plays once, one cycle, queued at once, and the evaluation returns when its notes are sent, as an imperative language's does. A clip must end, so as a clip either renders one cycle; `Language::setLive` was added for this. An expression is tried first as `once = (...) :: p.Pattern Int`, quietly.

- **Order.** The framework's live queue plays in the order sent and waits at a future event, so the adapter keeps every message in one heap and sends only what precedes `pat`'s next window.

- **Limits.** A 512 MiB heap and a 256 MiB stack (`funk_memory`): a runaway evaluation replies "out of memory" or "stack overflow" and the host goes on. Afterwards the host keeps 217 MiB resident, against 89 MiB before the evaluation: the empty chunks within the 128 MiB threshold stay.

- **The lib modules** are compiled into the plugin and written to a temporary directory at start-up: a bundle cannot rely on a funk checkout.

- **Tests.** `tests/hosts/clap`: `test_clap_host` with `tests/hosts/clap.txt`: evaluation, clips, Stop of a busy loop, state, Live, real time. funk has no clip that never ends, so its `forever` case exceeds the clip's event limit instead. All pass on macOS; Linux not run.

## Friction log

| # | Friction | Workaround | Fix candidate |
|-|-|-|-|
| 1 | `mapP :: (a -> b) -> Pattern a -> Pattern b` is rejected: the signature's arrows share an effect variable, and `Pattern`'s field needs `f` pure ([typed-effects.md](typed-effects.md), "A pure argument, regardless"). The message, "functions whose effects differ", names neither. | no signature | a pure-argument marker; or name the field and the parameter in the message |
| 2 | `map p.atom "abc"` fails: `String` is packed | `unpack` | the targeted error from the review |
| 3 | A field read on a parameter, `e.whole.start`, needs a signature: "cannot infer the record type" | a signature | infer from the field name when one type has it |
| 4 | A test file `pattern.fk` that imports `pattern` reports "import cycle: pattern -> pattern": the importer's directory is searched first and finds the file itself | rename | say "imports itself" |
| 5 | A definition keeps the names it was compiled with: after `:l`, an earlier `step` still plays the old `pat` | redefine `step` after each load | none needed; document for live use |
| 6 | Every `funk_eval` input stays compiled: about 16 KB each, and the slowest call grew from 0.12 to 0.62 ms over 40,000 calls. A host cannot evaluate per window | `funk_call` | rolling back an expression's chunk after it runs |
| 7 | `chain k named = ... g.Node (k "k_saw") ...`, with `k :: String ->> FunPtr ...`, is rejected under the signature `(String ->> FunPtr ...) -> Bool ->> g.Sig`: "functions whose effects differ: a signature fixes one, and another is required" (met in an earlier `tests/kernels/build.fk`). Without the signature it compiles. Possibly #1 again | no signature | as #1 |
| 8 | A function an export calls cannot return a tuple: it is a heap value. `envStep` computes a stage and a level | write both through the state pointer | unboxed multiple returns in exported code |
| 9 | A loop is a self-recursive function taking every loop invariant as a parameter: `svfLoop` takes 10 | none | a local loop closing over the invariants, compiled without a closure |
| 10 | `d.sine 0.25` is a type error: `Num g.Sig` takes `Int` literals, and funk has no `Fractional` | `d.dc 0.25` | `Fractional`, or `fromFloat` in `Num` |
| 11 | `tests/kernels/dsp.fk` shadowed `lib/dsp.fk` for the tests beside it: the importer's directory is searched first. As #4 | rename | warn when a module shadows one on the `-M` path |
