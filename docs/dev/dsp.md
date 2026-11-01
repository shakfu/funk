# DSP capabilities

Status on 2026-10-31. Design and measurements: [audio.md](audio.md), option D, and [261029-dsp-measurements.md](261029-dsp-measurements.md).

funk has 21 unit generators. [roadmap.md](roadmap.md) lists none to add, and `TODO.md` and `audio.md` mention only a stereo reverb, as an example. [Proposed roadmap](#proposed-roadmap) below is a proposal; its tasks are in `TODO.md`, under DSP.

## Unit generators

All 20 are in `lib/kernels.fk`. Costs are from `audio.md`, and for phases 1 and 2 and the kernels fused multiply-add changed from `build/dspbench` on 2026-10-31 (M1, `-O2`). A 64-frame block at 48 kHz lasts 1.33 ms.

| Group | Kernel | In `lib/dsp.fk` | Method | ns per 64 frames |
|-|-|-|-|-|
| Oscillator | `dsp_sine` | `sine` | C `sin` of a phase | 300 |
| | `dsp_saw` | `saw` | polyBLEP | 210 |
| | `dsp_square` | `pulse`, `square` | polyBLEP, variable width | 280 |
| | `dsp_noise` | `noise` | MINSTD generator, uniform in (-1, 1) | 425 |
| Filter | `dsp_lowpass1` | `lowpass1` | one-pole | 147 |
| | `dsp_lowpass`, `dsp_bandpass`, `dsp_highpass` | `lowpass`, `bandpass`, `highpass` | Cytomic trapezoidal state-variable filter, 12 dB/octave | 280 |
| Envelope | `dsp_adsr` | `adsr` | linear segments, restarted by a rising gate | 210 |
| | `dsp_adsr_exp` | `adsrExp` | exponential segments, restarted by a rising gate | 345 |
| Nonlinear | `dsp_saturate` | `saturate` | C `tanh` | 250 |
| Dynamics | `dsp_compress` | `compress`, `limit` | feedforward, peak level, C `pow` when over the threshold | 790 |
| Delay | `dsp_delay` | `delay`, `chorus` | memory, read between frames by linear interpolation | 125 |
| | `dsp_comb` | `comb`, `flanger`, `reverb` | feedback comb, with a one-pole low-pass in the loop | 230 |
| | `dsp_allpass` | `allpass`, `reverb` | Schroeder allpass | 170 |
| | `dsp_zita` | `zita` | Zita-Rev1, 8 lines; two nodes for stereo. Output matches Faust's within 1.4e-16, in double precision | 2000 per channel |
| Tables | `dsp_wavetable` | `wavetable`, `wavetableFile` | one cycle of a table, read between frames by linear interpolation | 240 |
| | `dsp_sampler` | `sample` | a file once per rising gate, linear interpolation | 125 |
| Utility | `dsp_const` | `dc`, numeric literals | constant | 4-6 |
| | `dsp_add` | `+`, `-` | sample-wise | not listed |
| | `dsp_mul` | `*`, `negate` | sample-wise | 4-6 |

polyBLEP is a correction near an oscillator's jump that reduces aliasing.

These add no kernels:

- **Channels:** `stereo`, `pan`.
- **Voices:** `poly`, `polyIx`, `polyOn`.
- **Graph nodes:** `g.control`, a value set by events, and `g.named`.

## Implementation

| Layer | Code | Runs on | Language |
|-|-|-|-|
| Unit generators | `lib/kernels.fk`, compiled into `funkdsp.so` | audio thread | funk, without its runtime; calls C `sin`, `tan`, `exp`, `tanh`, `pow`, `floor` |
| Graph builder | `lib/dsp.fk`, `lib/graph.fk` | funk's thread | funk, interpreted |
| Runner | `projects/funkgraph` | audio thread | C |

## Limits

- **One buffer per node.** A kernel gets one block of delay memory.
- **One output per kernel.** Channels that share state need two nodes with the same inputs, which evolve identically, each emitting one channel, at twice the cost: `zita` does this, and `g.Shared` gives each node all of its input's channels. `reverb` instead runs two separate reverbs, tuned 23 frames at 44.1 kHz apart.
- **A changed maximum restarts a delay.** Changing `delay`'s first argument zeroes its memory; changing the time input does not.
- **Feedback between nodes is one block late.** Feedback within a sample needs one kernel.
- **Separate kernels cost more.** A voice of separate kernels costs 2.7x a fused loop.
- **Some parameters are per block.** Filter cutoffs, envelope times and compressor settings are read once per block.
- **`limit` has no lookahead.** It holds its ceiling by turning each peak down within its frame, which distorts it.
- **Channels compress separately.** A stereo `compress` or `limit` follows each channel's own level, which can move the stereo image.
- **`wavetable` is not band-limited.** A bright table aliases at high notes.
- **`sample` reads WAV only.** Integer of 8 to 32 bits, or float of 32 or 64. It always has two channels; a mono file plays on both.
- **A replaced file plays from the next `sig`.** Nothing watches the file. Loading runs on funk's thread, so a large file can delay `pat`'s next window, but not the audio thread.
- **Noise needs a seed per voice.** Nodes with one seed play one signal. `polyIx` passes the voice's number, for a seed. Two instruments that both seed from it still match.

## Missing

This is an assessment of the code, not a roadmap.

| Missing | Examples | Needs |
|-|-|-|
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

A swap exchanges the buffers' pointers on the audio thread. `dspbench` measures the worst block with a swap of a 1 s delay at about 1 us, its clock's resolution, the same as without a swap. `kernels/graph` passes under AddressSanitizer and ThreadSanitizer.

Done 2026-10-31: `delay`, `comb`, `chorus`, `flanger`, `reverb` and all of them in series played in the plugin with no problems found.

### Phase 3: several outputs per kernel

Deferred 2026-10-31: the listen of phase 2 found no problem with the per-channel reverb. Start it only if a patch needs channels that share state.

A kernel writes several buffers, so channels can share state. The kernel signature changes, which affects every kernel and `projects/funkgraph`.

- Several outputs, in the signature and in graph expansion.
- A stereo reverb.

### Phase 4: loaded data

Tables are read-only samples, loaded on funk's thread and read by many nodes. A process-wide cache shares them by file and channel, or by content, and each is freed with the last graph that reads it.

- Shared read-only buffers: `projects/funkgraph/table.c`.
- A wavetable oscillator: `wavetable`, `wavetableFile`.
- Reading WAV files, and a sampler: `sample`.

`dspbench` measures the worst block with a swap to a graph reading a 1 s table at about 1 us, its clock's resolution, the same as without a swap. `kernels/tables` replaces a file 400 times while an audio thread renders, with no wrong samples, also under AddressSanitizer and ThreadSanitizer.

Done 2026-11-01: `d.sample` and `d.wavetable` played in `make clap-dev`, and replacing a file while it played, then evaluating `sig`, missed no block. The roadmap named a DAW; none was used.

### Later: only on demand

- FFT and spectral processing, which need their own block size.
- Granular synthesis, over phase 4's tables.
- Per-frame filter cutoffs, if per-block cutoffs step audibly.

### Decisions

A patch names a table by its file or its values, and never holds its pointer, so it cannot write to data other nodes read. A new read-only type was the alternative: funk has no finalizers, so a handle in a funk value could not tell the cache when to free. Compile-time constants were the other: simpler lifetimes, but replacing a file needs a recompile, and large samples pass through the compiler.

The WAV reader is about 60 lines of `table.c`, with every offset checked. miniaudio's decoders were the alternative, adding FLAC and MP3, but the plugin does not link miniaudio.

Generated kernels contract multiply-adds across statements; other generated C does not. Measured 2026-10-31 (M1, clang 21, best of 3, ns per 64 frames, off to fast):

| Kernel | Off | Fast | Ratio |
|-|-|-|-|
| `dsp_lowpass1` | 204.5 | 144.5 | 1.42 |
| `dsp_lowpass` | 335.6 | 275.4 | 1.22 |
| `dsp_comb` | 267.9 | 226.9 | 1.18 |
| `dsp_allpass` | 193.3 | 168.3 | 1.15 |
| `dsp_noise` | 482.9 | 422.5 | 1.14 |
| one voice, 12 nodes | 795.5 | 736.7 | 1.08 |

Over 10 s of filtered saw and noise, 1 of 480,000 output samples differed, by 3.6e-15, in `dsp_bandpass`; `dsp_lowpass1`, `dsp_lowpass` and `dsp_highpass` matched exactly. Kernels never run in the interpreter, so no compiled-against-interpreted comparison changes. Results can differ between targets: x86-64 builds without FMA instructions do not contract. An explicit `fma` was the alternative: it needs every kernel's arithmetic rewritten, and on such a target it calls a slow library function (inferred from the x86-64 baseline; not measured).
