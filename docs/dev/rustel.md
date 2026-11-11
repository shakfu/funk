# rustel: comparison note

Status: research, 2026-11-11. It compares funk with [rustel](https://github.com/tzfm/rustel) at commit `88156f8`, a native port of [Strudel](https://strudel.cc) in Rust. It records what funk could adopt for roadmap 0.13 ([roadmap.md](roadmap.md)) and for the open items T3, R6 and L7 ([tidal-comparison.md](tidal-comparison.md)). The tasks are in [TODO.md](../../TODO.md), under "From rustel".

## Where they stand

- **Language.** rustel runs Strudel's JavaScript in QuickJS. funk runs Tidal's Haskell syntax in its own interpreter and compiler.
- **Size.** One squashed commit, about 716,000 lines of Rust in 21 crates. The largest are the studio (254k), the runtime (135k) and the audio engine (77k). Maturity cannot be judged from the history.
- **Licence.** AGPL-3.0-or-later. funk is MIT. Ideas transfer; code does not.
- **Sound reference.** rustel compares its renders with Chrome's WebAudio (`crates/audio/tests/chrome_reference.rs`). funk's 0.13 reference is scsynth. rustel's DSP constants and kernels are therefore no reference for SuperDirt parity.

| | rustel | funk |
|-|-|-|
| Pattern language | Strudel, in QuickJS | Tidal 1.10, in funk |
| Time | reduced `i128/i128` fractions | 46126080 ticks per cycle (T1) |
| Sound | its own engine, after Strudel's superdough | SuperDirt over OSC; `d.kit` and voices in the plugin |
| Voices | a fixed-size record per voice; at most 512, newest dropped | a fixed graph; at most 64 voices, oldest stolen (`projects/funkgraph/graph.h:143`) |
| Orbits | 16, each with delay and reverb (`crates/audio/src/scalar.rs:232`) | S2, in 0.13 |
| Hosts | CLI, terminal studio, embeddable engine | REPL, `funk-midi`, CLAP plugin, `libfunk` |

## Lessons

### 1. Per-event effects as a fixed-order voice record

This bears on 0.13's design note: per-event chains in a fixed graph ([audio.md](audio.md) item 4).

rustel builds no graph per event. A voice is one fixed-size plain-data record, `OnsetEvent` with its `OscillatorControls` (`crates/audio/src/backend.rs:1132`). Each effect is an optional field. The voice renders them in one fixed order. A record may carry 3 extra `.FX()` stages, since the record must fit the lock-free ring; a pattern naming more is skipped with a log (`backend.rs:370-376`).

SuperDirt also applies its modules in one order: `DirtEvent.playSynths` calls `modules.do(_.value(this))` (`DirtEvent.sc:192`), in the order of `addModule` (`SuperDirt.sc:174`). Checked in the installed SuperDirt quark.

For SuperDirt's built-in modules, a fixed-order record matches SuperDirt with no change to the graph on note-on. It cannot express a module added by `addModule` or a user SynthDef. The design note must decide first whether parity covers those.

Alternative: per-event subgraphs from a pool, enabled through the event ring ([audio.md](audio.md) option D, item 4). This covers user modules, at the cost of graph changes while playing.

### 2. A test that the audio path does not allocate

rustel installs a global allocator that fails the test when the audio callback allocates, frees or locks (`crates/audio/tests/no_alloc_callback.rs`).

funk's checker proves exports allocation-free ([realtime.md](realtime.md), R4). The C around them is unchecked: `funkgraph`'s runner, `voices.c` and the plugin's `process()`. In C the same test interposes `malloc`, `free` and `pthread_mutex_lock`, and arms them while the callback runs. `tests/embed/overflow.c` already interposes `malloc` on macOS.

### 3. Negative controls in reference renders

Each case in `chrome_reference.rs` carries a wrong render that must fail the comparison. This shows the metric separates a right render from a wrong one.

rustel compares only peak and RMS. That is too weak for funk's exact parity. Keep the negative controls; compare samples against a stated error bound.

### 4. The scheduler's contract

`crates/scheduler/src/lib.rs:1-20` states five clauses:

- every event carries the generation of the pattern that made it, and an old generation is never delivered;
- each onset id is delivered once;
- stop has its own path, so evaluation cannot delay it;
- reuse of an old schedule expires after a set time;
- the clock is injected, and offline rendering uses the same virtual clock.

funk-midi's slots ([stream.md](stream.md)) and the plugin hold some of these without a written contract. Writing it gives each clause a test.

### 5. Refused edits

rustel keeps the last good score playing when an edit fails to evaluate. It also refuses an edit whose first rendered window has an invalid control (`docs/overview.md`, "Intentional differences"). funk already reports mini-notation errors at definition (L7). The second check would extend L7 to control values.

### 6. Bounded rationals (T3)

rustel's `Fraction` is a reduced `i128/i128`. It takes a 64-bit path while both parts stay below 2^31, and checks overflow: checked methods return `None`, the others panic (`crates/fraction/src/lib.rs:1-79`).

This is one option for T3, against Haskell's unbounded `Rational`. clang and gcc have `__int128`. Tidal's denominators can grow without bound, so the option needs a list of the Tidal patterns that would overflow it.

### 7. Editor integration without packages (R6)

`rustel play song.strudel --watch` reloads the file on save. Any editor then works with no package. `funk-midi --watch FILE` would close most of R6's editor gap.

### 8. Link (R6)

rustel follows Link's play and stop states, joins a running session, and releases MIDI clock input while joined (`docs/hardware.md#link`). Which Link library it uses was not checked. Ableton's Link SDK is GPL-2.0-or-later or commercial, so an FFI binding from MIT funk needs a licence decision first.

### 9. Smaller items

- **Voice steals.** rustel counts the onsets it drops past 512 voices (`scalar.rs:254-261`). `d.kit` steals within its 4 voices a name, where SuperDirt starts a synth per event. A count of steals shows when that difference is audible.
- **Session tapes.** rustel's studio logs each installed edit with its time, and replays the log (`docs/sessions.md`). This would make a live-set acceptance run repeatable.
- **Onset placement.** rustel keeps a sub-sample lead per onset (`backend.rs:1137`), for the browser. scsynth's `OffsetOut` places an onset on a whole frame, so parity wants whole frames. funk's rounding is unchecked.

## Not adopted

- rustel's DSP constants and kernels: they target WebAudio.
- The QuickJS layer, transpiler and terminal studio: funk has its own language and hosts.
- Peak and RMS as a parity metric.
