# SuperDirt's sound in the plugin: design

Status: option C implemented 2026-11-11; SuperDirt's core modules, and its orbits' global effects, 2026-11-12 (see Implementation). For roadmap 0.13 ([roadmap.md](roadmap.md#013----superdirt-sound-in-the-plugin)). It covers S1-S8 and S10 of [tidal-comparison.md](tidal-comparison.md), and item 4 of [audio.md](audio.md#d-a-graph-of-compiled-unit-generators-built-at-run-time). The decisions taken so far are at the end.

The question: how an event plays SuperDirt's chain of per-event effects, inside a graph that funk swaps only when code is edited, with no allocation on the audio thread. User modules, synths and global effects must be addable, as in SuperDirt (decided 2026-11-11).

Sources:

- SuperDirt, the installed quark at `7e245e8`, cited by file and line.

- SuperCollider, `supercollider/supercollider` on GitHub at `develop`, read 2026-11-11.

- rustel at `88156f8` ([rustel.md](rustel.md)).

## What SuperDirt does

For each event (`DirtEvent.sc:177-195`):

1. Sets every global effect of the event's orbit from the event's keys (`orbit.globalEffects.do { |x| x.set(currentEnvironment) }`). A changed key is sent to the effect; any key that is present resumes a paused effect (`GlobalDirtEffect.sc:40-58`).

2. Sends a cut message to the orbit's group, if `cut` is nonzero.

3. Makes a group for the event at the tail of the orbit's group.

4. Runs every module in order. A module whose test passes adds one synth to the group's tail (`core-modules.scd`). The order is the order of `addModule`. Replacing a module keeps its place; `orderModules` reorders them (`SuperDirt.sc:174-207`).

5. Adds `dirt_gate` last.

The synths share the orbit's `synthBus`. The source writes to it, and each module reads it and replaces it (`ReplaceOut`). `dirt_gate` reads it, applies a sine-shaped envelope over `fadeInTime`, `sustain` and `fadeTime`, and multiplies by `amp * pow(gain.min(2) + overgain, 4)`. It writes the result to the orbit's `dryBus` with `OffsetOut`, and clears `synthBus` for the next event's group. Its envelope frees the whole group (`doneAction: 14`) (`core-synths.scd:177-187`). Because scsynth runs nodes in order within a block, one bus per orbit serves every event in turn.

Timing: a synth starts at the start of the block that holds its time stamp. `OffsetOut` delays its output by the offset within that block and carries the remainder into the next block (`server/plugins/IOUGens.cpp:2020-2075`). Modulators and envelopes inside the chain therefore start up to one block early; only the output is placed on its sample. The block is 64 frames by default (`include/server/SC_WorldOptions.h:48`).

Each orbit (`DirtOrbit.sc:59-65`, `GlobalDirtEffect.sc:24-29`):

- has `dirt_delay`, `dirt_reverb`, `dirt_leslie`, `dirt_rms` and `dirt_monitor`, each a synth placed after the orbit's group;

- reads `dryBus` and `globalEffectBus`; `dirt_monitor` sums them, limits the result, and writes it to the output;

- lets the first three pause themselves after silence (`DirtPause`, `SuperDirtUGens.sc:194-202`); `dirt_rms` and `dirt_monitor` always run.

Extension points a user has:

- **Synths:** any SynthDef, played by `s "name"`, given every event key that matches one of its controls (`core-modules.scd`, module `sound`).

- **Modules:** `addModule`, `addFilterModule`, `removeModule` and `orderModules`. A test is any function of the event.

- **Global effects:** `orbit.globalEffects_`, at run time.

- **Routing:** the `out_to` and `map_from` modules, between buses.

Limits: scsynth's node count defaults to 1024 (`SC_WorldOptions.h:41`), and its real-time memory to 8 MiB (`:49`). It allocates a synth's memory on the audio thread from that fixed pool.

## Requirements

1. The chain of each event is SuperDirt's: the same modules, in the same order, selected by the same tests.

2. Users add synths, modules and global effects, and reorder modules, from funk code.

3. No allocation, freeing or locking on the audio thread (R4, [realtime.md](realtime.md)).

4. An edit does not cut the events that are playing, and does not restart the global effects' tails.

5. Silent voices and paused effects cost close to nothing.

6. 64-frame blocks inside the plugin, whatever the host's block size, with control-rate inputs read once per block as each UGen does (S8).

7. Output is checked against scsynth's non-real-time renders.

8. Graphs of nodes stay as they are: a user still builds a `Sig`, plays it and edits it live ([audio.md](audio.md) option D). The event layer adds to them.

## The framing

"Built-in only" and "user modules" are not the real split. In funk the chain is built by funk code, on funk's thread, each time the code is edited. Any design below can take its list of modules from the user's code at that point. What differs is when the instances of a chain exist:

- **ahead of time**, a fixed pool of voices, each holding every module, built with the graph (A, B);

- **per event**, made when the event starts and freed when it ends, as scsynth does (C).

## Templates and graphs

SuperCollider has the same split. A SynthDef is a template; a Synth is an instance made from it per event.

**A graph** is what funk has now. It is a table in C memory, and each row is a node:

- a kernel pointer;

- the node's own copy of its state;

- its output buffer;

- the indices of its input rows.

`play` allocates all of it on funk's thread. Every node renders every block. An edit swaps the whole table.

Voices in a graph are copies made at build time. `kitWith 4 dir ["bd", "sn"]` writes 8 copies of the sample voice. Each has its own nodes, state and buffers, whether it plays or not. In A and B, every voice is such a copy.

**A template** is the same description without state or buffers:

- the kernels, in order;

- the wiring, as indices within the template;

- the initial state bytes;

- the memory sizes;

- which inputs are parameters, by name, such as `cutoff`.

funk builds templates on each edit, as it builds graphs. Nothing renders from a template.

**An instance** is made per event, on the audio thread:

1. It takes a free slot from the template's pool, allocated when the templates were built.

2. It copies the template's initial state into the slot.

3. It writes the event's parameters to its parameter inputs.

4. It renders its nodes in the template's order each block, until its gate ends.

5. It returns the slot.

```text
graph:      [bd.v1: src lpf hpf ... gate] [bd.v2: ...] [bd.v3] [bd.v4] [sn.v1] ...
            every row allocated at build, rendered every block

templates:  { sample, lpf, hpf, ..., gate }            built at edit, never rendered
pools:      { sample: 64 slots, lpf: 64 slots, ... }   allocated at build
events:     [ e1: sample(bd) -> lpf -> gate,  e2: sample(sn) -> gate ]
            made per event from slots, rendered until the gate ends
```

What both share:

- kernels and their signature;

- controls;

- the builder in `lib/graph.fk`, since a template is written from a `Sig`, as a graph is.

What differs:

| | Graph | Templates and instances |
|-|-|-|
| Memory committed | per node, at build | per pool slot, at build; shared by every sound |
| What renders | every node | the events that are playing |
| A chain | fixed when built | assembled per event, from the modules whose tests pass |
| An edit | replaces the graph | replaces the templates; playing events finish on the old ones |

The two run side by side. The graph holds orbits, global effects and the user's own `Sig`. The event layer renders into the orbits' buses before the graph's global effects read them.

## Options

### A: a fixed record per voice, built-in modules only

rustel's design ([rustel.md](rustel.md#1-per-event-effects-as-a-fixed-order-voice-record)). Each voice of `d.kit` holds SuperDirt's 13 core modules, each bypassed when its control is absent.

- Pros: the least work, since it extends today's `d.kit`.

- Cons: fails requirement 2. Every voice renders every module.

Not considered further.

### B: a fixed pool of voices, each with every registered module

Each voice of each sound holds every registered module, built-in and user, in order. Per event, funk's thread runs the tests and sends a mask of the modules that pass, with the parameters. The runner skips a voice that is silent and a module whose bit is clear.

Pros:

- **Small change to the runner.** It keeps today's swap, node naming and voice controls ([audio.md](audio.md), item 4). The new parts are skipping and the mask.

- **Fixed cost and memory**, known when the graph is built.

- **State across edits.** A playing voice keeps its state through a swap, as named nodes do now.

Cons:

- **S10 stays.** Voices per sound are fixed, so a fifth overlapping event of a name steals the oldest. SuperDirt has no per-name limit.

- **The graph grows** as sounds × voices × modules × channels. 20 sounds, 4 voices, 13 modules of about 2 kernels each, and stereo make about 4,000 nodes (estimate). Swap and naming time at that size are not measured; 128 voices once took 50 ms to name in funk ([audio.md](audio.md), item 4).

- **Memory scales with modules.** Each module with memory, such as `pshift`'s delay, is allocated for every voice of every sound.

- **Stale parameters.** An event must reset every masked module's controls, or a voice keeps the last event's `cutoff`.

- **Skipping is new.** Today the runner calls every node every block.

### C: chains made per event, from templates and a pool

The graph splits in two:

- **Static part.** Orbits, global effects and the user's own `Sig`. It is swapped on edit, as today.

- **Event layer.** It belongs to the player, not to a graph. Each event's chain is made from templates and freed when its gate ends.

How it works:

1. **Templates.** A source, a module or a global effect is a template: a small graph description built in funk at edit time, with its kernels, inputs, initial state and memory sizes. The user registers them in funk code, as `addModule` does.

2. **Event records.** On funk's thread, each event's tests run against its controls, as SuperDirt's do, and select the chain: a source template and the modules that pass, in order. The event record carries the chain's template ids and parameter values (T2, [delivery.md](delivery.md)).

3. **Instances.** On the audio thread, the runner makes the chain from a pool: fixed-size slots per template, sized when the templates are built, so taking one is O(1) and never calls `malloc`. It renders the chain on the orbit's bus in event order, then the gate. It returns the slots when the gate's envelope ends, as `doneAction: 14` does.

4. **Placement.** An event starts at its block's start. The gate offsets its output within the block, as `OffsetOut` does.

5. **Pool exhaustion.** The newest event is dropped and counted (decision 2).

Pros:

- **SuperDirt's model.** A chain per event, only the modules that pass, and no per-name voice limit. S10 goes away: the limit is the pool's, as scsynth's is its node count.

- **Cost follows the events.** Typical events have 0 to 3 modules.

- **Templates are extension.** User synths, modules and global effects are templates too.

- **Edits are cheap.** Templates are built once per edit, not per voice, so the static graph stays small.

- **Edits do not cut playing events.** Playing events keep their template until they end. A swap replaces templates for new events only.

Cons:

- **A new runner layer.** It needs templates, a pool, an event list, orbit buses, the gate and 64-frame blocks. Today's runner is 443 lines (`projects/funkgraph/graph.c`); this may double it (estimate).

- **Kernel lifetime.** A kernel library must stay loaded while any event plays it, after the edit that replaced it.

- **New tests.** An event layer is concurrent code that the existing tests do not cover. It needs the no-allocation test ([TODO.md](../../TODO.md), "From rustel") and the stress tests under AddressSanitizer and ThreadSanitizer that `kernels/graph` has.

- **No state across edits for event nodes.** They are short-lived, so this matters only for long sustains.

### Comparison

| | A | B | C |
|-|-|-|-|
| User modules and synths | no | yes, at edit | yes, at edit |
| S10, voices per name | fixed | fixed | pool-wide, as scsynth |
| Cost of a silent voice | full | near zero, with skipping | none |
| Graph size | sounds × voices × 13 | sounds × voices × modules | templates |
| Change to the runner | small | medium | large |
| Matches SuperDirt's structure | partly | partly | yes |

## Node state other than `Float`s

[audio.md](audio.md) item 4. Today a node's initial state is a list of `Float`s; pointers and sizes come from `funk_graph_memory` and the table calls. SuperCollider's UGens need integers (RNG seeds, buffer positions), arrays (`vowel`'s formants) and memory sized from inputs (`pshift`, `CombL`).

- **S1. An init export per kernel.** `k_init(state, in, cx)` fills the state from the first values of the inputs and the sample rate. This follows SuperCollider's constructor per UGen. funk gives only the parameters; the state's layout is the kernel's `foreign struct`.

- **S2. A typed state builder in funk.** funk writes the state's bytes from the `foreign struct` declaration. The layout stays visible to funk's thread, which is unnecessary.

- **S3. Encode in `Float`s.** Exact only to 2^24, so a 32-bit seed does not fit.

Recommendation: S1. The memory a kernel needs is then a function of its inputs, declared by the kernel and allocated with the template's slot.

## Recommendation

C, with S1.

B also meets requirement 2. It keeps S10's difference, though, and pays for every module in every voice. 0.13's goal is SuperDirt's sound "with exact parity", and C is the design whose structure is SuperDirt's: a chain per event, on a bus per orbit, from a fixed pool, as scsynth allocates from its fixed pool.

The cost is a larger runner. Steps 1 and 2 of the plan measure it before any UGen is ported.

## Implementation

Done 2026-11-11, as option C:

- **C:** `projects/funkgraph/events.c`, with its API in `graph.h`. A template is a `funk_graph` built by `lib/graph.fk`'s builder and not played. A set of templates owns pools of slots, allocated when it is given to the player. A stage is laid out in a free slot on the audio thread: its states copied, its pointers set, its delay memory zeroed.

- **Size classes:** templates whose sizes round up to one power of two share a pool, its slots as large as the largest of them. A class has 256 slots in the hosts, or fewer if they would pass 8 MB, scsynth's default real-time memory (`memSize`). Then the classes with the fewest slots get one more at a time, the smaller slot first, while all pools fit in 128 MB, the `memSize` of `SuperDirt.start` (`classes/extSuperDirtStartup.sc`). For `d.superDirt` at 44.1 kHz the pools take 128 MB a player, from 63 MB. A `pshift` stage takes 196 KB at 44.1 kHz, so `pshift` has 42 slots. One pool sized by the largest template took 50 MB for 256 slots.

- **Graphs:** `g.Bus o c` reads an orbit into a played graph; `g.In c` reads the chain into a template. A graph with no bus has every orbit added to its output.

- **funk:** `lib/dsp.fk`'s `Dirt`, with `superDirt`, `loadSoundFiles`, `addSynth`, `addModule`, `addFilterModule`, `removeModule`, `orderModules`, `has`, `prepare`, `playDirt`, `orbit` and `orbits`. `d.prepare` marks an event with `~NAME` for each module whose test passes; `c.writeControlsWithBy` sends every number of a marked event. A module may add controls to an event it runs on, as SuperDirt's module function computes its arguments: `vowel` adds its formants.

- **Hosts:** funk-midi and the plugin load a top-level `dirt` as they play `sig`. `funk_voices_trigger` sends a trigger whose sound has a template to the event layer.

- **Kernels:** `funk_context` gains `done`, which ends an event, as `doneAction: 14` does. A sample's template is `dsp_dirt_span`, DirtEvent's `calcTimeSpan`, then `dsp_dirt_sample` on each channel, `dirt_sample_N_2`. `dsp_gate`, `dirt_gate2`, ends every event. They run SuperCollider 3.13.0's UGens in float arithmetic, constructors included: 3.13.0's `Line`, `LFSaw` and `EnvGen` each take a step in theirs, and `develop`'s do not all take one.

- **Modules:** `d.superDirt` has SuperDirt's 12 core effect modules, in its order, each a kernel running its SynthDef's UGen graph: `dsp_dirt_vowel`, `_shape`, `_hpf`, `_bpf`, `_crush`, `_coarse`, `_lpf`, `_pitch_shift`, `_envelope`, `_grain_envelope`, `_tremolo` and `_phaser`. `dumpUGens` gives each graph. Three behaviours of scsynth's they copy:
  - sclang sends a nil argument as the integer 0 (`OSCData.cpp`, `addMsgSlotWithTags`), so an argument a module names but the event lacks is 0, not the SynthDef's default.

  - scsynth constructs a synth's UGens at its first calc (`Graph_FirstCalc`), after the synths before it in the group have written the bus. `In`'s constructor reads the chain's first frame, and `RLPF`, `RHPF` and `BPF` filter it once in theirs; `PitchShift` stores it.

  - Built with NEON, nova-simd divides by `vrecpeq_f32` and one `vrecpsq_f32` step, about 16 bits (`vec_neon.hpp`). `shape`'s audio-rate `/` runs there; funk computes the ARM ARM's `FPRecipEstimate`. Likewise `EnvGen_next_ak_nova` fills a linear segment longer than the call with four float lanes.

- **Orbit effects:** templates named `o.NAME` and `a.NAME` are each orbit's effects, after the orbit's events in each block, as `GlobalDirtEffect` synths after the orbit's group. `d.superDirt` has `dirt_delay` (sc3-plugins' `SwitchDelay`), `dirt_reverb`, `dirt_leslie` and `dirt_monitor`; `d.superDirtCore` has the delay as SuperDirt makes it without sc3-plugins, a `CombL`. Inputs 0 and 1 read the orbit's dry bus, 2 and 3 its effect bus; an effect replaces both, the monitor gives the orbit's output. The layer runs the orbits from the last, as `DirtOrbit.initNodeTree` adds each group at the head.
  - **Pausing:** a kernel that sets `done` pauses its effect from the next block, as `PauseSelf` and `DetectSilence` do with `doneAction: 1`. An event resumes an `o.` effect it has a control of, and an `a.` effect always, with `resumed` set for a block, as `GlobalDirtEffect.set`. It sets a control that differs from the last such event's, to 0 if absent. `d.prepare` adds `dry`, `lock` and `delayAmp` as SuperDirt's event has them, so every event resumes the delay and the reverb, as in SuperDirt.

  - **A new set** keeps an effect whose name and graph, states included, are unchanged: the audio thread moves it from the set it replaces. Redefining `dirt` keeps the reverb's tail and its random delays.

- **Random numbers:** `funk_context` gains `rgen`, the player's scsynth world `RGen` (Taus88, `SC_RGen.h`), and `block`, scsynth's `mBufCounter`. `dsp_dirt_rand` and `dsp_dirt_lfnoise1` draw from it, chained so the reverb draws in its SynthDef's constructor order, and `pshift` draws at each quarter window. `funk_player_seed`, `g.seed` in funk, seeds it as `RandSeed` does; a player is seeded from the time, as scsynth's world is.

- **Level:** `dirt_gate` computes `amp * pow(min(gain, 2) + overgain, 4)` in floats, so `dirt` events play at SuperDirt's level. The monitor's limiter at 1, softclip and `DetectSilence`'s threshold act on absolute levels, so a louder funk could not match them. `d.kit` keeps its level 1 at `amp` 0.4, 8 dB above.

- **scsynth's details the global effects copy:** `dirt_monitor`'s `Limiter` outputs zeros for two buffers, after its constructor's frame, so an orbit is 881 frames late at 44.1 kHz. `LocalIn` reads 0 after a block the reverb was paused. A delay line is a power of two long, and `CalcDelay` clips to it, not to the maximum: the reverb's last `AllpassN`s draw up to 0.15 s against a maximum of 0.05. `DelayC` reads down to a frame's delay, as an `InterpolationUnit`. nova-simd ramps a changed control over a block in four float lanes. `LeakDC`'s coefficient is a float.

- **Synths:** `tests/dirt/synthdefs.scd` writes a SynthDef's UGen graph, as `SynthDef.children` holds it, to `lib/synthdefs.fk`: its controls and, per UGen, its class, rate, special index, outputs and inputs. `lib/scsynth.fk` builds a template from it: a control is the event's parameter of that name, with the SynthDef's default; a constant is a `dsp_u_K` node; each other UGen is a node of kernel `dsp_u_CLASS`, one per output, whose state starts with the UGen's index, rate, special index, output and its inputs' rates. The kernel picks the calc function scsynth's constructor would for those rates; at control rate it fills its block with its one value. `Out`'s inputs are the template's channels. `d.superDirt` and `d.superDirtCore` add `default-synths.scd`'s synths that every UGen of is ported for. Three details:
  - **Construction:** a node marked by `funk_graph_ugen`, as graph.fk marks every `dsp_u_` kernel, is called once for 1 frame with `ctor` set, before its stage's first block, in node order, as `Graph_FirstCalc` runs constructors before the first calc. A constructor reads its inputs' first frame: the controls' values and the earlier UGens' constructor frames.

  - **Random order:** a UGen that draws from the world `RGen` reads the last such UGen before it, so they draw in the SynthDef's order.

  - **Probes:** each UGen's arithmetic is checked against scsynth with a SynthDef of it alone, rendered by both. They found that `GVerb`'s gains are a double `pow` rounded once, where libm's `powf` misses by an ulp; that UnaryOp's `exp`, `log`, `sin`, `cos` and `tanh` are libm's float functions; and that a control-rate `LinXFade2` outputs with its last gain, its slope a whole frame.

  - **Demand rate:** a `Duty` whose level is a `Dseq` of constants folds the sequence into its state; other demand UGens are not ported.

  - **Arithmetic:** a UGen's `a * b + c` is unfused. SuperCollider.app 3.13.0's arm64 build fuses some, the global effects' `Rand` and `AllpassN` among them, but not `Pulse`'s: a probe matched scsynth only unfused, and fused arithmetic grew `supertron`'s feedback to 1e-5.

  - **Feedback:** `LocalIn` is named `fed:K` and `LocalOut`'s input `feed:K`; the event layer copies one into the other at each block's end (`graph.h`). `LocalOut`'s nodes are channels after the template's two, so they are built.

  - **sclang's random constants:** `superhammond`'s phases and `superfm`'s presets are drawn as the SynthDefs are built. Both `synthdefs.scd` and `render.scd` seed sclang's generator first, so they draw the same.

  - **`doneAction: 2`** sets `funk_context`'s `freeself`: the stage is skipped from its next block and the chain passes it unchanged, as a freed synth leaves the synth bus to `dirt_gate`. `Pan2` reads its level once a call and ramps it, so a synth that played on would leak a block.

- **Exports:** a template's node named `>NAME` sets parameter NAME of the stages after it, each block. The span exports `sustain`, `fadeTime`, `fadeInTime`, `speed` and `endSpeed`, so modules and the gate read DirtEvent's values. The alternative, the span on funk's thread as sclang computes it, needs each sample's length there.

- **Tests:** `kernels/events` (funk, interpreted and compiled), `kernels/templates` (C, plain and under AddressSanitizer and ThreadSanitizer), `hosts/events` (funk-midi) and a case in `hosts/clap`. `lib/dirt-scsynth` (`tests/dirt/oracle.py`) renders 48 cases in scsynth with SuperDirt 1.7.3's SynthDefs, through DirtEvent's own `calcTimeSpan` and `playSynths`, SuperDirt's module functions and 16 orbits of its own `GlobalDirtEffect`s, into a Score, seeded by `RandSeed`. 18 cases are modules and 14 global effects, among them a pause and resume, two orbits, and `pshift` with `psdisp` beside the reverb. funk matches every frame within 1.8e-7, against 0.017 or more for its output a frame late. Its scsynth times are a quarter frame past each event's frame: an OSC time tag is whole 2^-32 s, and scsynth truncates the offset.

Where it differs from SuperDirt, or from the plan:

1. **More than 2 channels.** A file's first two play; SuperDirt `Splay`s more into two.

2. **Parameter writes to a playing event take effect at its next block**, as scsynth's `n_set` does between blocks. Only cut groups write them.

3. **Late events.** The layer renders whole blocks ahead of a render call's end. An event that arrives after its block rendered starts at the next block, undelayed, and is counted. In the plugin this happens when the host's block is not a multiple of 64 frames.

4. **The played graph still renders in the host's blocks**, not scsynth's 64-frame grid, so requirement 6 holds for events only.

5. **An event under 8 frames still sets its orbit's effects.** SuperDirt drops it before `playSynths`; funk knows a sample's sustain only in its span's kernel.

6. **Routing.** The `out_to` and `map_from` modules, and `dirt_rms`, which sends levels to editors, are not ported.

7. **Memory.** A size class may hold fewer events than scsynth's memory would: scsynth allocates a `PitchShift`'s delay line by its window; funk sizes the slot for the largest window, 0.18 s. The orbit effects take about 34 MB a player at 44.1 kHz, mostly the delay's 4 s lines; SuperDirt's setup raises scsynth's `memSize` for the same. A `GVerb` node takes about 1 MB of delay lines and gives both outputs (`funk_graph_outputs` in `graph.h`): `superprimes` has 9 and `superwavemechanics` 8, so each takes a 9.8 MB slot, and their class gets 7 slots of the 128 MB. scsynth at its default 8 MB could not play one.

8. **Synths not ported:** `in`, `in1` and `inr` read the audio input, which an event has no access to; `pmsin` reads the mouse; `dirt_from` and `superpanic` route and free; `debug` prints. `cyclo`'s `cycle` is 0 unless the event gives it: funk's records do not carry Tidal's cycle. The extra synths and effects are step 6's remainder.

9. **`numWireBufs`:** the comparison raises scsynth's to 1024, as `superprimes` needs more than SuperDirt's startup sets, 64; with 64, SuperDirt does not load it.

10. **A UGen no output reaches is not built.** scsynth runs every UGen of a SynthDef; a template has only the nodes its channels reach. No ported synth has such a UGen; `LocalOut` and a `Rand` read by nothing would.

11. **A played graph has no constructor call:** a `dsp_u_` kernel there constructs itself at its first call, from its inputs' first frame of that call.

12. **`funk_player_event_audio` formats names with `snprintf`** on the audio thread, as `funk_voices_trigger` does. No test yet shows the audio path allocates nothing ([TODO.md](../../TODO.md), "From rustel").

13. **The FFT effects are within 2e-5 of scsynth, not 1e-6.** scsynth's macOS build transforms with vDSP, in float; `dsp_u_Spectral` transforms in double, to float buffers. 8 of the 10 FFT cases are within 5e-7. `spectral-conformer` is within 1.5e-6, as close as funk's double and float transforms are to each other. `spectral-enhance` differs by 9.7e-6, at one bin that scsynth's rounding moves across a step of `SC_Complex.h`'s polar table. Calling vDSP on macOS would need a C FFT the kernels can reach; Linux builds, which use FFTW, could not match either way.

## Plan

Steps 1 to 5 are done, and of step 6 the API, the UGen library and `default-synths.scd`'s synths; see Implementation. Step 2 left `d.kit`'s `dsp_dirt` as it was: linear interpolation and a linear pan.

1. **Event layer, on the virtual clock.** Templates, the pool, orbit buses, 64-frame blocks, the gate with its offset. One template, `d.kit`'s sample voice, on one orbit. Test: its output equals today's `d.kit` for single events. Also the no-allocation test, and stress tests under AddressSanitizer and ThreadSanitizer.

2. **S1 init exports.** Port `dirt_sample`'s `BufRd` with cubic interpolation (S5), `DirtPan` with `Pan2` and `Balance2` (S4), and the gate. Check against scsynth's non-real-time render.

3. **The plugin and `funk-midi --audio`** play control patterns through the event layer. `d.kit`'s fixed voices remain for notes (`m.kit`).

4. **The 13 core modules** (S1) and their UGens (S8), each checked against scsynth. Done 2026-11-12: `sound` and 12 effects.

5. **Orbits and global effects** (S2): pausing and resuming as `DirtPause`, and parameters set by events. Done 2026-11-12.

6. **Synths** (S3), and the funk API for user templates, with SuperDirt's names (decision 3): `addModule`, `addFilterModule`, `removeModule`, `orderModules`, synths and global effects. Done 2026-11-12: the API, the UGen library, `default-synths.scd`'s 5 portable synths and all 32 of `default-synths-extra.scd`'s, 46 `lib/dirt-scsynth` synth cases within 1.2e-7. Done 2026-11-13: `default-effects-extra.scd`'s 19 effects, 24 cases: 14 within 1.1e-8, the 10 FFT ones within 2e-5 (difference 13).

7. **AIFF** (S7) and `unit "s"` (S6). Done 2026-11-13: 5 `lib/dirt-scsynth` cases, exact.

## Open

- Whether the played graph should render on scsynth's 64-frame grid too (difference 4).

## Decisions

Taken 2026-11-11.

1. Graphs of nodes stay, and templates are added beside them (requirement 8).

2. A full pool drops the newest event and counts it. Stealing the oldest was the alternative. It cuts a sound that is already playing.

3. The funk API uses SuperDirt's names: `addModule`, `addFilterModule`, `removeModule`, `orderModules`.

Taken in the implementation, 2026-11-11, for the user to review:

4. A module's test is any funk predicate on `Controls`, as SuperDirt's is any function. It runs on funk's thread when the event's records are written, so it costs the audio thread nothing. A list of names would have let C run it, but not `coarse > 1`.

5. The hosts' pool has 256 stages per set: a sound or a module of an event. SuperDirt runs 2 or more synths an event in scsynth's 1024 nodes.

6. Samples load when `dirt` is defined, from the names `d.loadSoundFiles` gives, as `d.kit` loads them. SuperDirt loads every folder of a path.

7. Option C is implemented (decision 1).

Taken 2026-11-12, by the user:

8. Size classes for the pool, 8 MB each, then more slots within 128 MB in all.

9. Global effects run per orbit in the event layer, not in `sig`.

10. `dirt_monitor` is ported with its limiter and so its 20 ms; the alternatives were sending events 20 ms early, or no monitor.

11. scsynth's `RGen` is ported, seedable, so the oracle checks the reverb frame by frame.

12. Both delays: `SwitchDelay` in `d.superDirt`, the `CombL` in `d.superDirtCore`.

Taken in the implementation, 2026-11-12, for the user to review:

13. `dirt` events play at SuperDirt's level, 8 dB below `d.kit`'s, so the monitor's nonlinear stages match (see Implementation, Level).

14. SuperDirt's synths are its SynthDefs' UGen graphs as data, built by one library of UGen kernels (`lib/scsynth.fk`), not a kernel per synth as the modules and global effects are. 50-odd UGens serve the 50-odd synths and effects, the graph is SynthDef's own, and `superfm`'s random presets come with it. The cost: one node per UGen, and a control-rate UGen fills its 64 frames.
