# Audio: design sketch

Status: sketch, 2026-10-28. No code in the repo. funk's music library plays MIDI only ([../music.md](../music.md)); the sound comes from a synth outside funk. This note sketches four ways to make sound: A, a compiled synth driven by live patterns; B, A with the synth recompiled on edit; C, MIDI to external synths, as today; D, a graph of compiled unit generators built at run time, as sapf does. D is recommended (2026-10-29).

## What exists

- **Allocation-free exports** ([realtime.md](realtime.md), R4). A `foreign export` runs on an audio thread. The compiler proves it allocates nothing, so the collector never pauses it.

- **C-memory types** an export may use: `Array` with `readArrayWrap` and `writeArrayWrap`, `Ring`, `Atomic`, foreign structs.

- **`examples/ffi/portaudio.fk`**, a wavetable sine through PortAudio. At 64 frames and 48 kHz a callback has 1.33 ms; the longest took 4-6 us, while funk's thread collected garbage.

- **`tests/ffi/sched.fk`**: patterns to an export through a `Ring Int`, via `pushOnsets`. The export writes impulses.

## The constraint every option shares

An export may use ([realtime.md](realtime.md), "R4"):

- scalars, `Ptr`, and full-arity calls to functions of those types;

- `peek`, `poke`, foreign struct fields, `Array` with index wrapping, `Ring`, atomics;

- C imports without `String`;

- `if`, `case` on scalars and `Bool`, strict local values;

- recursion only as a call to itself in tail position, which compiles to a loop.

It may not build lists, closures, constructors or strings, or apply a function partially.

So audio-rate code is not built from values the way patterns are:

- **State** lives in a foreign struct in C memory.

- **A unit generator** is a function from that state and its inputs to a sample.

- **A signal graph** is the code of the render loop, within one export. A graph assembled at run time must live in C memory and be walked by C code (D).

This voice compiles under that check and runs on `tests/ffi/fkt.c`'s audio host. It has a saw oscillator, a linear envelope and a one-pole low-pass, and takes note-on and note-off from a ring. Note 57 set 220 Hz, and the envelope returned to 0 after the note-off.

```haskell
foreign export ccall "fk_voice" process :: Ptr () -> Ptr CFloat -> CInt ->> ()
foreign import ccall "math.h exp2" exp2 :: Float -> Float

foreign struct Voice
  { events :: Ring Int  -- from funk's thread: sample * 65536 + value
  , pos :: Int          -- samples played
  , phase :: Float
  , freq :: Float
  , gate :: Int
  , env :: Float
  , lp :: Float
  }

envStep :: Float -> Int -> Float
envStep e gate = if gate == 1 then min 1.0 (e + 0.004) else max 0.0 (e - 0.0001)

voiceSample :: Ptr Voice ->> Float
voiceSample v = do
  ph <- readField v Voice.phase
  writeField v Voice.phase (wrap (ph + readField v Voice.freq / 48000.0))
  e <- envStep (readField v Voice.env) (readField v Voice.gate)
  writeField v Voice.env e
  lp <- readField v Voice.lp + 0.1 * (saw ph * e - readField v Voice.lp)
  writeField v Voice.lp lp
  lp

render :: Ptr Voice -> Ptr CFloat -> Int -> Int ->> ()
render v out i n = if i == n then () else do
  takeEvents v (readField v Voice.pos + i) -- note-on/off due by this sample
  s <- voiceSample v
  pokeByteOff out (i * 4) (toCFloat (0.2 * s))
  render v out (i + 1) n
```

`saw`, `wrap`, `mtof` and `takeEvents` are a few lines each. The excerpt, with them, compiles under the check as printed. One friction already: a foreign struct takes one field per line, where a `data` record accepts `phase, freq :: Float`. Per sample, the code reads fields, computes, and writes fields back. That is C's style written in funk, checked for allocation. Writing this style is the main unknown in funk's case for DSP, and the first friction log for audio should measure it.

## C: MIDI to external synths (today)

- **How:** patterns produce MIDI (`funk-midi`, the CLAP plugin), and a hardware or software synth makes the sound.

- **Live:** everything funk does, since funk does no audio-rate work.

- **Gains:** no new code. Any synth works.

- **Limits:**
  - Sound design happens outside funk.

  - Timing is MIDI's: `funk-midi`'s sender is within 0.3 ms on macOS, plus the synth's own latency.

  - The note, velocity and channel encoding has no room for continuous parameters.

- **Next steps within C:**
  - control-change patterns, such as a cutoff that follows a pattern;

  - MIDI clock out, so a DAW follows funk's tempo.

## A: a compiled synth, driven by live patterns

```text
funk's thread (interpreted, live)            audio thread (compiled, fixed)
  pat --sampleOnsets per block-->  Ring Int  --> process: take due events,
       encode, ringPush                            render voices, write buffer
```

- **Live:** patterns, as in `funk-midi` now. The synth is fixed for a run.

- **Timing:** sample-accurate. Each event carries its sample, and the voice applies it at that sample. `tests/ffi/sched.fk` already checks the transport's exactness.

What it needs:

1. **A DSP library**, `lib/dsp.fk`, in the export subset:
   - oscillators: saw, square, and a sine table;

   - an ADSR envelope;

   - filters: one-pole, and a state-variable filter;

   - a fixed number of voices with an allocation policy.

   Voices sit in one C block, voice `i` at `plusPtr base (i * Voice.size)`: foreign structs cannot hold C arrays ([realtime.md](realtime.md)).

2. **Compiled code inside an interpreting host.** The host runs patterns in the interpreter, but the synth must be compiled. `funk emit-c` pastes the runtime into its output, so linking that output into `funk-midi` would put two copies of the runtime's symbols in one process. The fix is shared-library output with the runtime's symbols hidden ([embedding.md](embedding.md), phase 0 item 5):
   - `funk build --shared dsp.fk` builds a library with its own private runtime;

   - the host loads it with `dlopen` and calls `fk_voice`;

   - the two runtimes share only C memory: the voice struct and the ring. Their ring layouts agree when both come from one funk version.

3. **Audio output in `funk-midi`.** A `--audio` option, with PortAudio, which the example already uses, or miniaudio. Its callback calls `fk_voice`.

4. **Wider events.** A value has 16 bits, and note, velocity and channel need 18. Parameters such as cutoff need more. Options:
   - two ring entries per event, read as a pair; that needs a two-entry commit, or the reader checks the count;

   - a ring of a foreign struct, which needs `Storable` for structs;

   - 32 bits of sample offset within the block and 32 of value.

**Risks:**

- The export style may prove too clumsy beyond one voice. The cost per voice is unmeasured; the sine example's 4-6 us per 64-frame block is the only figure.

- A CLAP instrument needs audio ports, which `midi-langs`' framework does not have: it handles note effects only.

## B: A, with the synth recompiled on edit

- **Live:** patterns, and the DSP code.

- **How:** an edit to the DSP module goes through four steps:
  1. compile the module to a shared library;

  2. load it;

  3. switch the audio callback to it at a block boundary;

  4. unload the old library once the audio thread has left it.

  The interpreter already compiles, caches and loads foreign-import glue this way (`ffi.c`). B extends that to exports.

Edit latency, measured (M1):

| What is compiled | `-O2` | `-O1` | `-O0` |
|-|-|-|-|
| `tests/ffi/sched.fk`, a whole program: 6,485 lines of C, mostly the pasted runtime | 1.03 s | 0.58 s | 0.18 s |
| Foreign-import glue only, warm ([ffi.md](ffi.md)) | 87 ms | | |

A DSP module compiled without the runtime would likely land near the glue's figure, given its size. That is an estimate.

What it needs beyond A:

1. **Swapping without a glitch.**
   - The audio thread reads the callback through an atomic pointer.

   - The old library is unloaded after an epoch passes: a counter the audio thread advances once per block.

   - If the voice struct's layout changed, the new code needs fresh state. A short crossfade, say 10 ms, from the old library's output to the new one's avoids a click.

2. **`$CC` at run time.** Fine for a desktop live-coding setup. Plugins and Eurorack ship without a compiler, and iOS forbids loading new code. libtcc compiles in milliseconds but makes slower code ([261027-lang-review.md](261027-lang-review.md), option 2).

3. **Errors.** A module that fails the allocation check, or fails to compile, leaves the old synth playing, as a failed `pat` does.

**Risks:**

- Sound stops for a moment if a swap is slow.

- State is lost across a layout change.

- Every edit costs a compile, so B suits instrument design more than per-beat live coding.

## D: a graph of compiled unit generators, built at run time

sapf's model, which is also SuperCollider's server's. In sapf, evaluating `800 0 sinosc .3 * play` builds C++ generator objects wrapped in lazy lists. `play` gives them to a new player, whose render callback pulls them one block at a time (sapf's `src/engine/backends/CoreAudioBackend.cpp`; upstream [lfnoise/sapf](https://github.com/lfnoise/sapf)). `ZRef` changes a parameter, and `Plug` swaps a running input under a spinlock (sapf's `include/Object.hpp`). Interpretation happens once per expression; per-sample loops are compiled C++.

```text
funk's thread (interpreted, live)               audio thread
  graph value --play--> node table in C memory  --> C runner: per block,
  (kernels loaded on demand or ahead of time)        for each node: kernel(state, ins, out, n)
```

- **Unit generators** are funk exports with one C signature, such as `void k(void *state, const float **in, float *out, int n)`, plus their state's size. They are compiled ahead of time, or on demand while playing. The interpreter cannot run exports, so they are always compiled.

- **The graph** is built in funk on funk's thread, interpreted: a pure value, such as `sinosc 800 |> mul 0.3`, that `play` writes into a table in C memory. A row holds a kernel's address, its input rows, its state and its output buffer. Buffers and state are allocated then, so the audio thread allocates nothing; sapf allocates each block (inferred from `Gen::produce` and `List`'s allocation paths, not traced).

- **The runner** is C in the host, about 100 lines (an estimate). Each block it calls every row's kernel in order. A new graph replaces the old one through an atomic pointer at a block boundary; funk's thread frees the old table once the audio thread has passed an epoch it advances per block.

- **Live:** patterns, rewiring and parameters at once; a new kernel after a compile.

Why the graph is in C memory, not funk's heap: exports may not use heap values, and the collector could free a graph the audio thread still reads unless it is pinned. `Array`, `poke` and foreign structs already write C memory from interpreted code.

What it needs:

1. **Kernels without the runtime.** `funk emit-c` pastes the runtime, so a kernel loaded into an interpreting host brings a second copy. Code in the export subset uses little of it: `fk_ffi_enter` and `fk_snap_lo` around C imports (by code reading; unverified as a full list). Without the runtime's 6,000 lines, a kernel should compile near the glue's 87 ms (an estimate), not 1.03 s.

2. **A C runner**, since funk cannot call through a `FunPtr`: it has `&cname` imports but no `"dynamic"` import. The alternative is a `"dynamic"` import that R4 allows.

3. **A kernel library**, `lib/dsp.fk`: A's oscillators, envelope and filters, in the kernel signature.

4. **A graph builder** in funk, and voices: a note-on adds rows, or enables rows from a pool, through the event ring.

Limits:

- **Feedback within a sample** across nodes is impossible; a node reads its input's previous block. A filter with such feedback is one kernel.

- **Node outputs go through memory.** Code compiled per sample, as Faust's, keeps them in registers. The cost per node is unmeasured.

- **Graph changes come at pattern rate**, from funk's thread, not at audio rate: enough for voices driven by MIDI or patterns.

LLVM, as Faust uses it, stays a later option for kernels: only if compiling a kernel without the runtime through `$CC` proves too slow.

## Comparison

| | C | A | B | D |
|-|-|-|-|-|
| Patterns live | yes | yes | yes | yes |
| Synth live | outside funk | no | yes, about 0.1-1 s per edit | rewiring at once; a new kernel after a compile |
| Sound designed in funk | no | yes | yes | yes |
| `$CC` at run time | no | no | yes | only for new kernels |
| Feedback within a sample | in the synth | yes | yes | inside one kernel |
| New funk work | none | DSP library, shared-library output, audio in `funk-midi`, wider events | A, plus compile-and-swap | kernels without the runtime, kernel library, C runner, graph builder, wider events |
| Runs in a plugin | yes (note effect) | needs audio ports in the framework | needs `$CC` in the plugin's process | with kernels built ahead of time; needs audio ports |

## Recommendation

D. It gives live DSP edits without a compiler in the loop for rewiring, runs in a plugin with kernels built ahead of time, and needs no new dependency. A is D with one fixed graph; B is D's on-demand kernels applied to the whole synth.

Order:

1. Kernel output without the runtime, and shared-library output with hidden symbols for plugins (`embedding.md` phase 0 item 5).

2. `lib/dsp.fk` in the kernel signature, and the wider event encoding.

3. The C runner with the graph swap, then the graph builder in funk.

4. `funk-midi --audio`.

5. Measurements: the cost per node and per voice, how many voices fit in 1.33 ms, and a kernel's compile time. Log the friction of writing kernels.

## Open questions

- The kernel signature: several outputs, inputs at control rate, and a kernel's parameter count.

- Polyphony: a fixed number of voices with stealing, or one voice per pattern layer?

- Parameters: events on the ring, or atomics the audio thread reads every block?

- Where the synth is defined: one file built ahead of time, or a section of the live file that B recompiles?

- In a CLAP instrument, does `midi-langs` grow audio ports, or does funk get a plugin of its own?
