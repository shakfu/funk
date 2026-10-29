# A plugin with audio output

Status: implemented, 2026-10-30: B, on a copy of `midi-langs`' framework ([Decision](#decision)); the plugin has a stereo output. gh:3. The question in [audio.md](audio.md#open-questions): does `midi-langs`' CLAP framework grow audio ports, or does funk get a plugin of its own?

Line counts below are measured; costs are estimates from reading the code, not from doing the work.

## What exists

- **`midi-langs`' framework** (`projects/clap`, 2,959 lines; `plugin.cpp` 787): one `plugin.cpp` behind about ten language adapters. It gives an editor (pugl and imgui), UDP input, clip and live modes, state, offline render, a dev host and a test host.
  - It is a note effect (`CLAP_PLUGIN_FEATURE_NOTE_EFFECT`): one MIDI output port, no note input.

  - It already answers `CLAP_EXT_AUDIO_PORTS`, with 0 ports (`plugin.cpp:659`).

  - `process()` emits MIDI events only; it never touches `audio_outputs`.

  - `Language` (`language.hpp`, 40 lines) has `eval`, `nextWake`, `resume` and `stopVoices`; nothing runs on the audio thread.

  - One instance per process (`gInstanceExists`, `plugin.cpp:765`), because interpreters keep process-wide state.

  - Neither `dev_host.cpp` nor `test_clap_host.cpp` passes audio buffers.

- **funk's adapter**, `midi-langs/projects/funk-midi/funk_clap.cpp` (352 lines): runs `libfunk` on its own thread and queues every call to it.

- **What `funk-midi --audio` already does** (`projects/funk-midi/main.c`, 787 lines): owns a `funkgraph` player, loads `funkdsp.so`, has funk build graphs with `g.play`, renders on the audio thread through `funk_player_render_channels` alone, and turns `pat`'s notes into voice controls (`voice_*`, about 80 lines).

- **`libfunk`**: one session per process ([embedding.md](embedding.md), phase 1 not done). Since gh:1, two different plugins that each embed `libfunk` run side by side, and their foreign imports bind their own runtimes.

## Constraints on every option

1. **One funk instance per process.** A DAW track count of one, per plugin binary, until embedding.md's phase 1. Two binaries (a note effect and an instrument) can coexist.

2. **The audio thread runs C only.** Rendering is `funk_player_render_channels`, which allocates nothing; funk never runs there. This holds today in `funk-midi`.

3. **Kernels must be found.** `lib/graph.fk` looks kernels up with `dlsym` in the player's libraries. A plugin exports only `clap_entry`, so kernels linked into the plugin binary are not found that way. Either ship `funkdsp.so` inside the bundle (`Contents/Resources`) and `dlopen` it, or give the player the kernels' addresses directly.

4. **`funkgraph`'s functions** reach `lib/graph.fk`'s imports through `funk_provide`, as `funk-midi` does.

5. **gh:2 (stack overflow in compiled code ends the process) is not due.** The plugin runs interpreted code, which reports overflow as an error, and kernels. The export check rejects any recursion but a function's tail call of itself, which compiles to a loop (`cgen.c:617`, test `error/ffi_export_recursion`), so a kernel's stack depth is bounded by its call graph. gh:2 becomes due only if a plugin runs a whole compiled program.

## Options

### A. Audio ports in `midi-langs`' framework

The framework gains an opt-in audio output; funk's adapter uses it.

- `LanguageInfo` gains the output channel count (0 for every other language) and the feature list (`instrument` and `synthesizer` when it has outputs).

- `Language` gains `render(float *const *out, uint32_t channels, uint32_t frames)`, called from `process()` on the audio thread. The default does nothing.

- `kAudioPorts` reports one stereo main output when the count is 2.

- funk's adapter moves `funk-midi`'s player, kernel loading and voice code into `funk_clap.cpp`; `pat`'s notes drive the voices inside the plugin, and still go out as MIDI.

- `test_clap_host` passes output buffers and checks a rendered peak, as `hosts/audio` does.

Cost: about 100 lines in the framework, 200-300 in the adapter, 50-100 in the test host.

Gains:

- Reuses the editor, UDP, clip and live modes, state and offline render: about 2,900 lines.

- Other languages are unchanged by default, and any of them could later render audio.

Risks:

- A shared framework of about ten plugins changes; its tests must cover every adapter.

- Clip mode renders events ahead in beats; audio from a graph is rendered per block. A clip of notes driving voices works; an offline render of a live graph needs `tl_.frontier`-style waiting for graph swaps too.

- The plugin lives in `midi-langs`, so a funk change that touches it needs both repos, as now.

### B. funk's own plugin, `projects/funk-clap`

A new project in this repo: CLAP headers vendored in `thirdparty/clap`, a plugin with a stereo output and a note input, built into `build/`.

- Minimum: descriptor, audio and note ports, `process()` calling the player, state (the source text), activation. Code arrives by UDP or a watched file; no editor.

- Reuses `projects/funk-midi`'s player, kernel and voice code by moving it into a library both link.

Cost: about 600-900 lines without an editor; an editor like `midi-langs`' adds about 600 (pugl, imgui, `editor_*`).

Gains:

- One repo: the plugin, `libfunk` and `funkgraph` change together, and `make test` covers them.

- Note input from the DAW is natural: an instrument receiving MIDI, with patterns optional.

Risks:

- Duplicates what `midi-langs` has: editor, UDP, clip mode, offline render.

- Without an editor, live coding inside a DAW needs an outside editor sending UDP.

### C. Two plugins: the note effect, plus an instrument

Keep `midi-langs`' `funk-midi` as a note effect. Add an instrument that renders graphs and takes MIDI in; the DAW routes the effect's MIDI to it.

- The instrument embeds its own `libfunk` session (allowed since gh:1) to build graphs, or none, if it plays a fixed graph chosen by name.

- It is B without patterns, or A with a second adapter.

Gains:

- The note effect stays as it is.

- Patterns can drive any synth, and the funk synth can be played from any MIDI source.

Risks:

- `sig` and `pat` live in two sessions: one cannot read the other's values.

- Routing between tracks is per DAW, and some DAWs cannot route a plugin's MIDI output (from general knowledge of DAWs, not checked here).

### D. No plugin

`funk-midi --audio` runs beside the DAW; audio reaches it through a loopback device. No work. No transport sync, no project state, no offline render.

## Comparison

| | A | B | C | D |
|-|-|-|-|-|
| New code (est.) | 350-500 lines | 600-1,500 | A or B, plus routing | 0 |
| Editor in the DAW | yes | only with +600 lines | yes for the effect | no |
| Clip mode, state, offline render | yes | to write | effect only | no |
| Repos changed per funk change | 2 | 1 | 1 or 2 | 1 |
| Note input from the DAW | to add | yes | yes | no |
| Other languages affected | opt-in only | no | no | no |

## Recommendation

A. It needs the least new code, and the framework already declares the audio-ports extension. Most of the work, the player and voices, exists in `funk-midi` and moves rather than being written.

## Decision

B, with `midi-langs`' framework copied into funk rather than rewritten. A would change a framework of about ten note-effect plugins to suit one language; `midi-langs` stays MIDI only.

Done:

- `projects/clap`: the framework, copied, with `midi_clock` and `midi_open` from `midi-langs`' `projects/common`. CLAP, pugl and imgui are vendored in `thirdparty/`.

- `projects/funk-clap`: the adapter, moved from `midi-langs/projects/funk-midi`. The plugin's id is unchanged.

- `-DFUNK_CLAP=ON` (`make clap`) builds `build/clap/funk-midi.clap`; `tests/hosts/clap` runs `test_clap_host` on it.

The copy removes B's costs for the editor, UDP, clip mode, state and offline render. It costs drift: a fix to either copy does not reach the other.

Next: audio ports in `projects/clap`, as A describes, but in funk's copy only.

## Notes: MIDI and voices both

Decided 2026-10-30: `pat`'s notes go out as MIDI and play the plugin's voices. Every note the plugin sends, clip or live, passes through one closure, `emit` in `projects/clap/plugin.cpp`, on the audio thread, with its frame in the block. `emit` also gives each note to the voice pool before the block renders. Clip mode, the host's transport, offline render and Stop's note-offs then reach the voices with no code of their own, at exact frames; `funk-midi --audio` instead estimates frames from its clock. MIDI out stays, for outside synths.

A note sent to an outside synth too plays twice. A `sig` without `d.poly` has no voices, so a user who wants MIDI only leaves them out. Rejected for now: splitting by MIDI channel (`m.chan`), voices on one channel and MIDI on the rest; it can be added later.

Steps, all done:

1. `funk_player_send_audio`, a queue for events made on the audio thread. `funk_player_send`'s queue has one producer, the host's thread, which `g.send` already uses. Rendering merges the two by frame.

2. `funk_voices` in `funkgraph` (`voices.c`): `funk-midi`'s voice code, now shared. It takes a note's off frame when known (`funk-midi`), or a later note-off (the plugin's MIDI).

3. `AudioOutput` in `projects/clap/language.hpp`: an optional table of audio functions in `LanguageInfo`. With it, the plugin lists the instrument feature and one stereo main output, and `emit` hands each note to it before rendering. Without it, a plugin is a note effect as before.

## The plugin's audio

- **`sig`** plays on the output from the next block, as in `funk-midi --audio`; `voices f` is `d.poly` of 8 voices. Notes on every MIDI channel share them.

- **Kernels**: `funkdsp.so` is copied into the bundle's `Resources` (beside the `.clap` on Linux) and loaded at the first activation, since the plugin's own symbols are not exported (constraint 3).

- **graph.fk's imports** are compiled with `$CC` when the plugin loads; `graph.h` is embedded and written beside the modules. Without a C compiler, or without `funkdsp.so`, MIDI still plays and the output is silent, with a message: the modules without foreign imports, `pattern` and `midi`, needed none before.

- **Activation** defines `audioPlayer` at the host's rate on funk's thread, from the host's main thread, and plays `sig` again; a new rate builds graphs for it.

- **Test**: `tests/hosts/clap`'s `[audio]` case. A clip's note sounds from frame 0 to its note-off at frame 24000, 448 frames into a block.

## Open questions

1. Does the plugin take note input from the DAW, as an instrument? Its notes could reach the same voice pool through `funk_player_send_audio`.
