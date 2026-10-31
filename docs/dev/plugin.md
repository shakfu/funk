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

- **`libfunk`**: up to 256 sessions per process ([embedding.md](embedding.md#phase-2-sessions)). Since gh:1, two different plugins that each embed `libfunk` run side by side, and their foreign imports bind their own runtimes.

## Constraints on every option

1. **One funk instance per process.** A DAW track count of one, per plugin binary, until embedding.md's phase 1. Two binaries (a note effect and an instrument) can coexist. Lifted 2026-10-30: [instances](#instances).

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

- **`sig`** plays on the output from the next block, as in `funk-midi --audio`; `voices f` is `d.poly` of 8 voices, and `voicesOn c f` is `d.polyOn c` of 8 for MIDI channel `c`. A channel's notes play its own voices if the graph now playing has them, else the shared ones. The check is per note-on, not per definition, so a channel with `voicesOn` in a replaced `sig` falls back as the new graph plays. A note-off searches every pool for its channel and pitch, so a note still ends after the graph changed which pool its channel uses.

- **Kernels**: `funkdsp.so` is copied into the bundle's `Resources` (beside the `.clap` on Linux) and loaded at the first activation, since the plugin's own symbols are not exported (constraint 3).

- **graph.fk's imports** use glue linked into the plugin ([below](#glue-without-a-compiler-at-load)), so the plugin runs no C compiler. If the linked glue does not match, it is compiled with `$CC` as before, with `graph.h`, embedded, written beside the modules. Without `funkdsp.so`, or without both, MIDI still plays and the output is silent, with a message.

- **Activation** defines `audioPlayer` at the host's rate on funk's thread, from the host's main thread, and plays `sig` again; a new rate builds graphs for it.

- **Test**: `tests/hosts/clap`'s `[audio]` case. A clip's note sounds from frame 0 to its note-off at frame 24000, 448 frames into a block.

## Glue without a compiler at load

Glue is the C that `ffi_load` writes for a module's `foreign import`s: one wrapper per import, which unboxes the arguments, calls the C function and boxes its result. It is compiled with `$CC`, loaded with `dlopen`, and cached in `~/.cache/funk/ffi` by a hash of its source, the loading binary's path, the compiler arguments and the libraries. The kernels need none: they are compiled when the plugin is built. graph.fk's 14 imports need it: 782 lines, about 70 ms to compile, 51 KB.

Fixed, 2026-10-30: the plugin wrote its modules to a new `mkdtemp` directory on each load, which its `-I` names, so the cache key changed and every load compiled the glue again. They go to `$XDG_CACHE_HOME/funk/plugin/<hash of the files>`, kept across loads and written only when missing, so `graph.h`'s time never makes cached glue stale. Option 1 below then removed the compile; this keeps the fallback cached.

Options to need no compiler at all:

1. **Glue compiled when the plugin is built.** Done, 2026-10-30; see [Linked glue](#linked-glue). The build emits the glue for the embedded modules and links it in; libfunk looks it up before compiling. Needs in `ffi.c`:
   - wrapper names independent of load order: they are numbered by the import's slot in the global table (`fk_ffi_%d` of `gidx`), which depends on what was loaded before;
   - a key without the loading binary's path and the compiler arguments, which differ between the build and the load;
   - a call to register linked-in glue, such as `funk_glue(key, bind, wrappers)`.

   It serves any module with imports, and graph.fk keeps one path. User code with its own imports still needs `$CC`.
2. **Graphs without foreign imports.** `g.play` pokes the graph into a host's buffer, as `pattern.writeEvents` writes events (`lib/pattern.fk:122`); the host builds it with `dlsym` and `funk_graph_add`. No glue, no libfunk change; kernel names must be encoded, `g.send` too, and graph.fk gets a second backend.
3. **Vendored libffi**: calls built at run time, for every host. A second calling path beside the glue, closures for callbacks, a new dependency.
4. **funkgraph's functions as libfunk primitives.** Rejected: ties the language to one library.

## Linked glue

`funk emit-c --glue FILE` writes the wrappers of the imports of FILE and the modules it imports, as C to link with libfunk, and `funk_glue_table`, each import's key and wrapper. A host passes the table to `funk_glue` before the code that imports them loads; `ffi_load` then takes each import whose key is there and compiles only the rest.

- **Key**: a hash of the import's wrapper, generated with a fixed id and without its funk name, and of its headers and structs. It no longer depends on load order (the wrapper's name was `fk_ffi_<gidx>`), on how the module is reached (`graph.graphNew` or another qualified name), or on the binary and compiler arguments. A change to the import's type, C name or header changes it, and that import is compiled again.
- **Binding**: the linked wrappers are in the host's binary with libfunk, so the runtime's functions and the host's, such as `funkgraph`'s, bind at link time: no `fk_glue_bind`, no `funk_provide`.
- **The plugin** emits `projects/funk-clap/glue.fk`, which imports the modules its session imports, at build time.
- **Tests**: `embed/glue` runs an import through linked glue with `CC=/nonexistent/cc`, after a global that moves its slot; `embed/glue/none`, the same without `funk_glue`, fails to compile. `hosts/clap/nocc` runs every plugin check, audio included, with `CC=/nonexistent/cc`, and compiles no glue.

## Note input

Done, 2026-10-30: an instrument plugin has a note input port, in the CLAP and MIDI dialects, and the host's notes play the same voices as the plugin's own. They are not sent on to the MIDI output, and funk code does not see them.

- **Order**: `process()` collects a block's notes, the host's and those `emit` sends, sorts them by frame, and only then gives them to `AudioOutput::note`. The voices take events in time order and move an earlier one up to the last frame sent, so a clip's note at frame 0 handled after a host's at frame 300 started at 300. The sort is an insertion sort: `std::stable_sort` may allocate, on the audio thread.
- **Capacity**: 1024 notes per block; more are dropped.
- **Tests**: `tests/hosts/clap` sends a CLAP note ended by MIDI and a MIDI note ended by a CLAP note, each sounding from its frame at its velocity, and a host's note at frame 300 in the block of a clip's note at frame 0. Without the sort, the last check fails.

## Instances

Done, 2026-10-30, once `libfunk` ran sessions side by side ([embedding.md](embedding.md#phase-2-sessions)): each plugin instance has its own funk session, player, voices and kernels handle, so a DAW may put the plugin on as many tracks as it likes, up to `libfunk`'s 256 sessions.

- `Language` gained the audio calls as virtual methods (`audioActivate`, `audioNote`, `audioRender`), so they reach their instance; `LanguageInfo` gives only the output's channel count, 0 for a note effect. Free functions in a table could not tell instances apart.
- `midi_clock.c`'s sink and clock are per thread: each instance's evaluator thread installs its own.
- funk's library files are written to the cache directory once per process: two instances would have raced on one temporary name, which carries only the pid.
- One UDP port serves one instance; a second one's bind fails, and the editor shows it. Test `hosts/clap` passes the port between two instances through their state's flag: a definition in one is not in scope in the other, and one note sounds through each one's own `sig`.

## Syntax colours

Done, 2026-10-30: the editor colours funk from `projects/tree-sitter-funk`'s parser and its `queries/highlights.scm`, the grammar Helix and Neovim use, so the three agree.

- ImGui's multiline field cannot colour parts of its text. The field keeps editing, undo, word wrap and the eval keys, and draws its text transparent; `DrawTokens` (`projects/clap/editor_colours.cpp`) draws the same text over it, a colour per byte, repeating ImGui 1.93's layout. A code editor widget such as ImGuiColorTextEdit was the alternative; it has no word wrap, and the eval keys and caret tracking would have moved to it.
- The framework has no tree-sitter: `LanguageInfo::highlight` gives a `Token` per byte, or is null for plain text. funk's (`projects/funk-clap/highlight.cpp`) parses the whole text on each change; a later pattern's capture wins, as in Neovim.
- It builds where `projects/tree-sitter-funk/src/parser.c` (`make tree-sitter`) and the tree-sitter runtime are found; elsewhere, such as CI, the text is plain. The plugin grows from 2.2 MB to 6.4 MB.
- Tests: `hosts/clap/colours` draws the field headless, by ImGui and by `DrawTokens`, and compares every glyph's vertices: wrapped or not, scrolled down or right, active, and at fractional font sizes. An ImGui update that moves its text fails it. `hosts/clap/highlight` checks a token per construct.

## Open questions

1. Should the host's notes reach funk code, for patterns that answer them, or the MIDI output?
