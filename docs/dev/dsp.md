# DSP capabilities

Status on 2026-10-31. Design and measurements: [audio.md](audio.md), option D, and [261029-dsp-measurements.md](261029-dsp-measurements.md).

funk has 15 unit generators. [roadmap.md](roadmap.md) lists none to add, and `TODO.md` and `audio.md` mention only a stereo reverb, as an example. [Proposed roadmap](#proposed-roadmap) below is a proposal; its tasks are in `TODO.md`, under DSP.

## Unit generators

All 15 are in `lib/kernels.fk`. Costs are from `audio.md`, and for phase 1's four from `build/dspbench` on 2026-10-31 (M1, `-O2`). A 64-frame block at 48 kHz lasts 1.33 ms.

| Group | Kernel | In `lib/dsp.fk` | Method | ns per 64 frames |
|-|-|-|-|-|
| Oscillator | `dsp_sine` | `sine` | C `sin` of a phase | 300 |
| | `dsp_saw` | `saw` | polyBLEP | 210 |
| | `dsp_square` | `pulse`, `square` | polyBLEP, variable width | 280 |
| | `dsp_noise` | `noise` | MINSTD generator, uniform in (-1, 1) | 480 |
| Filter | `dsp_lowpass1` | `lowpass1` | one-pole | 210 |
| | `dsp_lowpass`, `dsp_bandpass`, `dsp_highpass` | `lowpass`, `bandpass`, `highpass` | Cytomic trapezoidal state-variable filter, 12 dB/octave | 340 |
| Envelope | `dsp_adsr` | `adsr` | linear segments, restarted by a rising gate | 210 |
| | `dsp_adsr_exp` | `adsrExp` | exponential segments, restarted by a rising gate | 345 |
| Nonlinear | `dsp_saturate` | `saturate` | C `tanh` | 250 |
| Dynamics | `dsp_compress` | `compress`, `limit` | feedforward, peak level, C `pow` when over the threshold | 790 |
| Utility | `dsp_const` | `dc`, integer literals | constant | 4-6 |
| | `dsp_add` | `+`, `-` | sample-wise | not listed |
| | `dsp_mul` | `*`, `negate` | sample-wise | 4-6 |

polyBLEP is a correction near an oscillator's jump that reduces aliasing.

These add no kernels:

- **Channels:** `stereo`, `pan`.
- **Voices:** `poly`, `polyOn`.
- **Graph nodes:** `g.control`, a value set by events, and `g.named`.

## Implementation

| Layer | Code | Runs on | Language |
|-|-|-|-|
| Unit generators | `lib/kernels.fk`, compiled into `funkdsp.so` | audio thread | funk, without its runtime; calls C `sin`, `tan`, `exp`, `tanh`, `pow`, `floor` |
| Graph builder | `lib/dsp.fk`, `lib/graph.fk` | funk's thread | funk, interpreted |
| Runner | `projects/funkgraph` | audio thread | C |

## Limits

- **State is a list of `Float`s.** One second of delay at 48 kHz needs 48,000 elements, copied on each graph swap that keeps it. Not measured.
- **One output per kernel.** A stereo reverb whose channels share state cannot be written (`TODO.md`).
- **Feedback between nodes is one block late.** Feedback within a sample needs one kernel.
- **Separate kernels cost more.** A voice of separate kernels costs 2.7x a fused loop.
- **Some parameters are per block.** Filter cutoffs, envelope times and compressor settings are read once per block.
- **`limit` has no lookahead.** It holds its ceiling by turning each peak down within its frame, which distorts it.
- **Channels compress separately.** A stereo `compress` or `limit` follows each channel's own level, which can move the stereo image.
- **Noise needs a seed per voice.** `poly` gives a voice no index, so noise inside it plays one signal in every voice unless the seed comes from elsewhere.

## Missing

This is an assessment of the code, not a roadmap.

| Missing | Examples | Needs |
|-|-|-|
| Sources | wavetable, sampler | a way to load data into a kernel |
| Delay effects | delay, comb, allpass, chorus, flanger, reverb | delay memory |
| Envelopes | multi-segment | a new kernel |
| Spectral, granular | FFT, granular | large state and buffers; FFT also needs its own block size |

Subtractive polyphonic synths work now: oscillators, resonant filters, envelopes, audio-rate modulation of oscillator frequency and pulse width, panning and MIDI voices.

## Proposed roadmap

Each phase needs only the phases above it. Phase 2 is the largest step: it changes what a node's state can be.

Every new kernel gets a case in `tests/kernels/ugens.fk`, a function in `lib/dsp.fk`, and a cost in the table above.

### Phase 0: listen

Done 2026-10-31 (gh:4): the plugin played in a DAW with no problems found.

### Phase 1: kernels with no new mechanism

Done 2026-10-31: `noise`, `saturate`, `compress`, `limit` and `adsrExp` played together in a DAW.

### Phase 2: delay memory

Node state grows beyond a list of `Float`s, to a zeroed buffer of a given size. funk's thread allocates it, as it does other state, so the audio thread still allocates nothing. The audio thread also frees nothing, and the freeing code must not assume a buffer has one owner, because phase 4 reuses it.

- Delay memory in node state, kept across a graph swap without copying it on the audio thread.
- Delay, comb and allpass.
- Interpolated reads, then chorus and flanger.
- A mono reverb from combs and allpasses, one per channel through multichannel expansion.

Done when: a graph swap that keeps a 1 s delay misses no block at 64 frames, and `kernels/graph` passes under AddressSanitizer and ThreadSanitizer.

### Phase 3: several outputs per kernel

A kernel writes several buffers, so channels can share state. The kernel signature changes, which affects every kernel and `projects/funkgraph`.

- Several outputs, in the signature and in graph expansion.
- A stereo reverb.

Skip this phase if the per-channel reverb of phase 2 sounds good enough.

### Phase 4: loaded data

Immutable tables and samples, loaded on funk's thread and read by many nodes. These use a read-only type of their own, over phase 2's allocation and freeing.

- Shared read-only buffers, kept alive until the last graph that reads them is freed.
- A wavetable oscillator.
- Reading audio files, and a sampler.

Done when: a sampler plays a file in a DAW, and replacing the file during playback misses no block.

### Later: only on demand

- FFT and spectral processing, which need their own block size.
- Granular synthesis, which needs phase 4.
- Per-frame filter cutoffs, if per-block cutoffs step audibly.

### Decisions

Delay memory and loaded data share allocation and freeing, and have separate language-level types. One type would let a node write to a sample other nodes read, and the compiler could not reject it. The alternative for loaded data is compile-time constants: simpler lifetimes, but replacing a file needs a recompile, and large samples pass through the compiler. Reconsider that if phase 4 needs only wavetables and short samples. This design is not prototyped.

### Open decisions

- Whether to allow fused multiply-add in kernels (`TODO.md`). It affects every new filter's speed and its results against the interpreter.
