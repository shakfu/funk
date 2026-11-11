# SuperDirt's sound in the plugin: design

Status: proposal, 2026-11-11, for roadmap 0.13 ([roadmap.md](roadmap.md#013----superdirt-sound-in-the-plugin)). It covers S1-S8 and S10 of [tidal-comparison.md](tidal-comparison.md), and item 4 of [audio.md](audio.md#d-a-graph-of-compiled-unit-generators-built-at-run-time). The decisions taken so far are at the end.

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

## Plan

1. **Event layer, on the virtual clock.** Templates, the pool, orbit buses, 64-frame blocks, the gate with its offset. One template, `d.kit`'s sample voice, on one orbit. Test: its output equals today's `d.kit` for single events. Also the no-allocation test, and stress tests under AddressSanitizer and ThreadSanitizer.
2. **S1 init exports.** Port `dirt_sample`'s `BufRd` with cubic interpolation (S5), `DirtPan` with `Pan2` and `Balance2` (S4), and the gate. Check against scsynth's non-real-time render.
3. **The plugin and `funk-midi --audio`** play control patterns through the event layer. `d.kit`'s fixed voices remain for notes (`m.kit`).
4. **The 13 core modules** (S1) and their UGens (S8), each checked against scsynth.
5. **Orbits and global effects** (S2): pausing and resuming as `DirtPause`, and parameters set by events.
6. **Synths** (S3), and the funk API for user templates, with SuperDirt's names (decision 3): `addModule`, `addFilterModule`, `removeModule`, `orderModules`, synths and global effects.
7. **AIFF** (S7) and `unit "s"` (S6).

## Open

- The pool's size per template.
- scsynth's RNG for random UGens, and `float32` against a stated error bound (S8).
- Whether a module's test is any funk predicate on `Controls`, as SuperDirt's are any function, or a list of control names that must be present.
- Sample loading: event records name a sample, which must already be loaded on funk's thread. Today `d.kit` takes its list of names at build time.

## Decisions

Taken 2026-11-11.

1. Graphs of nodes stay, and templates are added beside them (requirement 8). Option C is the direction the user leans to; it is not yet decided.
2. A full pool drops the newest event and counts it. Stealing the oldest was the alternative. It cuts a sound that is already playing.
3. The funk API uses SuperDirt's names: `addModule`, `addFilterModule`, `removeModule`, `orderModules`.
