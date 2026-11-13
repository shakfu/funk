# Control patterns: delivery

Status: decided 2026-11-07, for 0.11's control patterns ([roadmap.md](roadmap.md#011----tidal-expressive-patterns)); step 1 of the plan is done. The decisions are at the end.

The question: how an event with named parameters, such as Tidal's `s "bd*2" # speed "<1 2>" # pan 0.3`, reaches MIDI output, the plugin's voices and `d.kit`.

Sources: Tidal's `tidal-core/src/Sound/Tidal/{Core,Pattern,Params}.hs` on `main`, downloaded 2026-11-07, cited by file and line. funk at the 0.11 work in progress.

## Today

Every hop from a pattern to the audio thread carries one `Int` per event, which becomes 3 MIDI bytes.

1. `pat` is a `Pattern Int`: note + 128 * velocity + 16384 * (channel - 1) (`lib/midi.fk`).

2. `p.writeEvents` writes 3 `Int`s per event, start, duration and value, up to 4096 per 10 ms window. The host calls `step` through `funk_call` (`projects/funk-clap/funk_clap.cpp:71`, `projects/funk-midi/main.c:60`).

3. The plugin turns each event into a note-on and a note-off in `pending_`, a heap of 3-byte messages, and sends them with `midi_out_send` (`funk_clap.cpp:623-645`).

4. The framework's `Timeline` holds `data[3]` per message (`projects/clap/timeline.hpp:19`). Each block, it emits CLAP MIDI events and calls `audioNote(offset, data)` (`projects/clap/plugin.cpp:471-529`).

5. `audioNote` calls `funk_voices_on` or `funk_voices_off`, which send `vK.gate`, `vK.freq` and `kN.gate` to the graph (`projects/funkgraph/voices.c`).

Two more paths exist:

- `p.pushOnsets` puts 16-bit values on a `Ring Int`, for `tests/ffi/sched.fk`.

- `funk_player_send(frame, name, value)` sets any named graph control from a given frame, from the host's thread or the audio thread. A name with no control is ignored (`graph.h`).

## Tidal

- `type ControlPattern = Pattern ValueMap`, `type ValueMap = Map.Map String Value` (`Pattern.hs:88`, `:1097`).

- `data Value = VS String | VF Double | VN Note | VR Rational | VI Int | VB Bool | ...` (`Pattern.hs:1079`).

- A control parses mini-notation into a one-key map pattern: `gain = pF "gain"` (`Params.hs:1378`). `Params.hs` defines 715 of them. `sound` is grouped: `"bd:3"` sets `s` to `bd` and `n` to 3 (`Params.hs:141`).

- `#` is `|>`: structure from the left, values from the right, by a union that prefers the right (`Core.hs:238`, `:261`).

- The other operators follow one scheme (`Core.hs:151-258`):
  - `|>|` and `|<|` take structure from both sides; `>|` and `<|` take it from the right.

  - `|<` keeps the left values.

  - `|+|`, `|+` and `+|` add the values of matching keys (`Map.unionWith`); `-`, `*`, `/` and `%` have the same forms.

- `Num ValueMap` exists: `fromInteger i` is `n` = `i` (`Pattern.hs:487-495`).

- SuperDirt receives one OSC message per event, with all its parameters. Each event plays its own synth instance, which reads them at its onset. Parameters belong to events, not channels.

## Requirements

1. **Per event.** A voice or sample reads its event's parameters at the onset. Two simultaneous events may differ: `[bd # pan (-1), sn # pan 1]`.

2. **No allocation on the audio thread** (R4, [realtime.md](realtime.md)).

3. **MIDI output stays**: note, velocity and channel, plus CC and pitch bend, for external instruments and for recording in the DAW.

4. **Names resolve on funk's thread**, as `m.kit` resolves them now.

5. **Errors at definition** where possible, as `p.mini`'s are.

6. **The slowest query stays well inside its window**, measured in the interpreter (0.11's acceptance test).

## Payload

- **P1: open keys.** `data Value = VF Float | VI Int | VS String`, and `type Controls = [(String, Value)]`, sorted by key. `type ControlPattern = Pattern Controls`.
  - Any parameter name works, as in Tidal, so a user's own synth can take its own.

  - A union is a merge, linear in the keys.

  - A string where a number belongs fails at delivery, not at definition.

- **P2: a closed record**, one `Maybe` field per known parameter.
  - Typed and fast.

  - A new parameter needs a change to `lib/`.

- **P3: integer ids** for names, from a table.
  - Faster lookup.

  - The table is needed anyway, and ids are worse to read in errors.

Recommendation: P1. It is Tidal's model, and events carry few keys. 2 to 4 keys is an estimate, not a measurement. funk has no pure map; `IntTable` in the prelude is effectful, which rules it out for pattern values.

The API this implies, in a new `lib/control.fk`:

- **Controls:** `c.s`, `c.n`, `c.note`, `c.gain`, `c.speed`, `c.pan`, `c.legato`, `c.velocity`, `c.midichan`, `c.ccn`, `c.ccv`, `c.midibend`. Each takes mini-notation, as Tidal's do. `c.param name` covers the other 700.

- **Operators:** `#` and Tidal's other forms, built on `apP`, `apLeft` and `apRight`. funk already parses `#` and `|+|` as user operators ([tidal.md](tidal.md)).

- **Numbers:** a float mode in the mini parser. Its ratio mode (`MRat`) already reads `1.5`, `3%2` and `q`.

- **Num:** `Num Controls`, as Tidal's.

## Transport

- **T1: MIDI only.** Each event's value stays one `Int`. Its parameters become MIDI messages at the note-on's frame: velocity, channel, CC (`ccn`/`ccv`, and a table such as `pan` → CC 10), and pitch bend. `audioNote` keeps each channel's CCs as controls `cC.ccN`, and a voice latches them at its note-on.
  - No framework change.

  - Values are 7 bits, or 14 with paired CCs.

  - Parameters are per channel. Simultaneous events on one channel with different values collide, which fails requirement 1, unless notes rotate over channels as MPE does.

  - `speed` 1.5 needs a scale into 0 to 127.

- **T2: event records.**
  - `writeEvents` writes per event: start, duration, note, velocity, channel, k, then k pairs of parameter id and float bits.

  - Ids come from a per-session table of names, built on funk's thread.

  - The plugin passes the parameters beside the MIDI bytes: `Timeline`'s message gains an index into a per-block parameter pool.

  - `funk_voices_on` takes the parameters. It sends each to the chosen voice's `vK.<name>` and the note's `kN.<name>` before the gate rises, so the voice or sample reads them at its onset.

  - MIDI output still gets note, velocity and channel, and T1's mapped CCs.

  - Meets requirements 1 to 3.

  - Costs: a change to `projects/clap`, which then diverges further from `midi-langs`' framework; a pool capacity per block; and control names formatted per event on the audio thread, into a stack buffer, with one `funk_player_has` lookup per parameter.

- **T3: graph controls only.** Each parameter goes to the graph control of its name, at the event's onset.
  - `funk_player_send` exists, so this is the least work.

  - One value per graph, not per voice, so it fails requirement 1 for anything polyphonic.

Recommendation: T2, with T1's mapping for MIDI output. T3 falls out of T2: a parameter with no per-voice control can go to the graph control of its name.

## Consumers under T2

- **Voices.** `voices` gains a form that reads parameters, such as `voicesWith ["cutoff", "pan"] (\v -> ...)`, where `v.gate`, `v.freq` and `v.param "cutoff"` are signals. Each reads control `vK.cutoff`, with a default.

- **`d.kit`.** Each sample reads `kN.speed`, `kN.gain` and `kN.pan`, latched at its trigger. `s` picks the key, as `m.kit` does now.

- **MIDI.** The note comes from `note` or `n`, the velocity from `velocity`, the channel from `midichan`; `ccn`/`ccv` and `midibend` send their messages. Whether `gain` should set the velocity when `velocity` is absent is open. I have not checked what SuperDirt's MIDI target does.

## Plan

1. `lib/control.fk`: `Value`, `Controls`, controls, operators, the float mode, and tests. No host changes.

2. Hosts: `writeEvents`' record format; `funk-midi` and the plugin read records; the MIDI mapping.

3. `projects/clap`: the parameter pool; `audioNote` with parameters; `funk_voices_on` with parameters; `voicesWith`; `d.kit`'s parameters.

4. Measure the slowest query with control patterns, in the interpreter, against 0.11's acceptance test.

Step 1 is useful alone: patterns can be written and tested before any host plays them.

## Decisions

Taken 2026-11-07.

1. Payload: P1, open keys.

2. Transport: T2, with T1's mapping for MIDI output, in the plan's order.

3. Controls keep Tidal's values and note names: in `c.note` and `c.n`, `0` and `c5` are middle C, and a consumer converts them. `n` is also a sample's index, so no fixed offset applies when parsing. `p.mini` keeps MIDI's numbering, `c4` being 60.

4. `control as c`, its operators imported by name; replaced the same day by `import control (..)`, below.

5. `pat` may be a control pattern; the hosts' type check grows, and a `Pattern Int` becomes controls inside the host.

## Step 1

`lib/control.fk`, tested by `tests/lib/controls.fk`:

- `Value`, `Controls` (sorted pairs), `get`, `fromList`, and `Num Controls`, with Tidal's arithmetic, quirks included: `|-` negates a name only on the right, and `|/` multiplies `Int`s, since Tidal's `recip` leaves them.

- The operators `+ - * / % ** ++ < >` in the forms `|op|`, `|op`, `op|` and `||op`, and `#`. `|>` is the prelude's `x |> f`, so `#`, its Tidal synonym, stands for it.

- `pF`, `pI`, `pS`, `pN`, `grp`, and Tidal's controls, generated from `Params.hs`: 196 single controls, 3 grouped (`sound`, `cc`, `nrpn`) and 58 aliases. The bus and recv forms and `array`, an OSC blob, are left out.

- In `lib/pattern.fk`, `miniF` and `miniN` read Tidal's `Double` and `Note`: decimals, ratio letters, and note names with `c5` 0. In `miniF` the ratio letters come first, so `gain "e"` is 1/8; in `miniN` the note names do. `miniTI` reads Tidal's `Int`.

- Importing `control` costs about 10 ms when a session starts.

## Step 2

Done 2026-11-07: control patterns play as MIDI in `funk-midi` and the plugin.

- `c.midiOf` follows SuperDirt's MIDI event (`DirtEventTypes.sc`, defaults from `DirtOrbit.sc`), on Tidal's `develop` branch as of 2026-11-07. `c.writeControls` writes three `Int`s a message: its time, its bytes with their size, and a note-on's length. The length rides with the note-on because `funk_voices` sends in time order: an off queued ahead of the next window would hold that window's notes back.

- The hosts choose `step`'s form by `pat`'s type, through `funk_type_of`. A `Pattern Int` keeps its own path rather than becoming controls, since the two produce the same messages.

- Messages keep their size from `step` to the output, so a program change sends 2 bytes and a clock 1.

- Every CC is sent by `ccn`/`ccv` or `midicmd`, as in SuperDirt; no control maps to a CC by default.

- Not sent: `midicmd`'s `sysex` and `smpte`, which need more than 3 bytes, the framework's limit, and `noteOn`, `noteOff` and `polyTouch`, whose defaults without a note were not checked.

- The alias `c` cost something not foreseen in decision 4: a session that imports `control as c` cannot use `c` as a variable, so `\c -> ...` fails. The hosts' own `voicesOn c f` failed so, and now says `ch`.

- The hosts now `import control (..)`, a new form ([modules.md](modules.md#imports)), so patterns read as Tidal's, `s "bd*2" # gain "0.8"`, and `c` is free. An unqualified import blocks only top-level definitions of its names, where an alias blocks the name as a variable too. `control.fk` exports its controls, operators, types and param makers, not its helpers, and its classes without their methods, so the session takes no common name such as `left` or `union`; `get`, `pairs` and `fromList` became `getControl`, `controlPairs` and `controlsFromList`.

## Step 3

Done 2026-11-08: an event's parameters reach the voices, and a sample event `d.kit`, in `funk-midi` and the plugin.

- `c.recordsOf` adds records to `writeControls`' stream: a name's chunks, 8 characters each, then a value, a `Float` in the payload's 8 bytes; and a trigger, which takes the chunks before it as a sample's key. A MIDI event's parameters precede its note-on, except the names `midiOf` reads.

- `funk_voices_on_with` sends each parameter to the note's voice, `vK.name`, before the gate rises; `funk_voices_trigger` sets `prefix.name` and pulses `prefix.gate` for one frame. Both use `funk_voices`' ordered send, since the player needs its events in time order (`graph.c:375`), and skip controls the graph lacks, so an event does not fill the 4096-event queue with names no node reads.

- The plugin carries them to its audio thread as 3-byte messages of statuses 0xF4, 0xF5 and 0xFD, undefined in MIDI 1.0: a parameter is a name's id and 32 bits of float in 7-bit pieces, a trigger a key's id. The framework passes them to `audioNote` and not to the host. This replaces the plan's pool beside `Timeline`: a clip loops its messages for as long as it plays, so a pool's entries would have needed the clip's lifetime; inline, the parameters are recorded with their note. The names live in append-only tables, 128 parameters and 4096 samples a session, written before the messages that use them.

- `d.kit` reads, for each sample, `s.bd:3.gate`, `.speed`, `.level` and `.pan`, so a control pattern names its sample rather than a note, and the 128-note limit does not apply. `m.kit` and `d.key` still play it by note.

- Since then, 2026-11-08: a sample plays as SuperDirt's `dirt_sample` and `dirt_gate` do, with `begin`, `end`, `loop`, `cut` and the rest (`dsp_dirt` in `lib/kernels.fk`), on 4 voices a name. `funk_voices_trigger` takes the voices in turn, and finds how many there are from the graph's controls, so `lib/dsp.fk` alone sets the number. A voice per event, as SuperDirt's synths, was the alternative; the graph is fixed while it plays, so voices must exist before the events that use them.
