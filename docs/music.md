# Patterns and MIDI

A guide to funk's music library: `lib/pattern.fk`, `lib/midi.fk`, and the two programs that play them. Design and measurements are in [dev/patterns.md](dev/patterns.md).

- **`pattern`** describes events over musical time. A pattern is a query: given a span of time, it returns the events in it. Patterns combine into larger ones.
- **`midi`** sets velocity and channel on a pattern of MIDI notes.
- **`funk-midi`** is a REPL that plays a pattern to a MIDI port and replaces it live when you redefine it.
- **The CLAP plugin** (`projects/funk-clap`) does the same inside a DAW, or in a standalone host.

The patterns drive an external synth over MIDI. `funk-midi --audio` also plays `sig`, a graph of `lib/dsp.fk`'s signals, to the audio device ([dev/audio.md](dev/audio.md), D).

Every output below comes from `funk-midi --no-sleep`, at the default cycle of 2000 ms. Note-offs are left out.

## Quick start

Build `funk-midi` with MIDI output, through the vendored `thirdparty/libremidi`:

```sh
cmake -B build -DFUNK_LIBREMIDI=ON
cmake --build build --target funk-midi
build/funk-midi
```

It opens a virtual MIDI port named `funk-midi`. Point a synth or a DAW track at it, then type:

```haskell
pat = p.fastcat [p.atom 60, p.atom 64, p.atom 67]
```

```text
    0.000 on  ch1 60 100
  666.666 on  ch1 64 100
 1333.333 on  ch1 67 100
```

`pat` plays from the next 10 ms window and repeats every cycle. Define it again to change what plays. `pattern` is imported as `p`, and `midi` as `m`.

## Time

- **A cycle** is the unit of musical time. A host gives it a length: 2000 ms in `funk-midi` by default, 4 beats in the CLAP plugin.
- **A tick** is 1/720720 of a cycle (`p.cycleTicks`). 720720 is the least common multiple of 1 to 16, so a cycle divides exactly into 2 to 16 steps, and into products of those while the prime factors last. A step of 1/17, or triplets nested three deep (1/27), rounds to the tick below.
- **Time is absolute.** Every pattern is aligned to cycle 0, which starts when the host starts. Redefining `pat` does not restart it: the new pattern plays where the old one would have been.

## Building patterns

| Function | Type | Plays |
|-|-|-|
| `p.silence` | `Pattern a` | nothing |
| `p.atom v` | `a -> Pattern a` | `v` once per cycle, lasting the whole cycle |
| `p.fastcat ps` | `[Pattern a] -> Pattern a` | each of `ps` in one equal step of every cycle |
| `p.cat ps` | `[Pattern a] -> Pattern a` | one of `ps` per cycle, in turn |
| `p.stack ps` | `[Pattern a] -> Pattern a` | all of `ps` at once |
| `p.fast k pat` | `Int -> Pattern a -> Pattern a` | `pat` `k` times per cycle |
| `p.slow k pat` | `Int -> Pattern a -> Pattern a` | one cycle of `pat` over `k` cycles |
| `p.shift t pat` | `Int -> Pattern a -> Pattern a` | `pat`, `t` ticks later; negative is earlier |
| `p.euclid k n v` | `Int -> Int -> a -> Pattern a` | `v` on `k` of `n` steps, spread evenly, the first on step 0 |
| `p.mapP f pat` | | `pat` with `f` applied to each value |

An event lasts its whole step, so notes are legato: each note-off coincides with the next note-on, and the note-off goes first.

### Sequences

`fastcat` divides a cycle into equal steps. `p.silence` is a rest:

```haskell
pat = p.fastcat [p.atom 36, p.silence, p.atom 36, p.silence]
```

```text
    0.000 on  ch1 36 100
 1000.000 on  ch1 36 100
```

Nest `fastcat` to subdivide a step:

```haskell
pat = p.fastcat [p.atom 36, p.fastcat [p.atom 38, p.atom 38]]
```

```text
    0.000 on  ch1 36 100
 1000.000 on  ch1 38 100
 1500.000 on  ch1 38 100
```

`cat` plays one pattern per cycle, in turn:

```haskell
pat = p.cat [p.atom 60, p.atom 62]
```

```text
    0.000 on  ch1 60 100
 2000.000 on  ch1 62 100
```

### Chords and layers

`stack` plays patterns together. A chord:

```haskell
pat = p.stack [p.atom 60, p.atom 64, p.atom 67]
```

```text
    0.000 on  ch1 60 100
    0.000 on  ch1 64 100
    0.000 on  ch1 67 100
```

A polyrhythm, 3 against 5 against 7, stays exact over any number of cycles:

```haskell
pat = p.stack [p.fast 3 (p.atom 60), p.fast 5 (p.atom 64), p.fast 7 (p.atom 67)]
```

### Speed and offset

```haskell
pat = p.fast 2 (p.fastcat [p.atom 60, p.atom 64])
```

```text
    0.000 on  ch1 60 100
  500.000 on  ch1 64 100
 1000.000 on  ch1 60 100
 1500.000 on  ch1 64 100
```

```haskell
pat = p.slow 2 (p.fastcat [p.atom 48, p.atom 53])
```

```text
    0.000 on  ch1 48 100
 2000.000 on  ch1 53 100
```

`shift` takes ticks. An eighth of a cycle:

```haskell
pat = p.shift (div p.cycleTicks 8) (p.fastcat [p.atom 60, p.atom 64])
```

```text
  250.000 on  ch1 60 100
 1250.000 on  ch1 64 100
```

### Euclidean rhythms

`p.euclid k n v` puts `k` onsets on `n` steps, as evenly as possible:

```haskell
pat = p.euclid 3 8 36
```

```text
    0.000 on  ch1 36 100
  750.000 on  ch1 36 100
 1500.000 on  ch1 36 100
```

The onsets fall on steps where `mod (i * k) n < k`. The rhythm is maximally even; its rotation can differ from that of Bjorklund's algorithm, which Tidal uses.

### Changing values

`mapP` applies a function to every value. Transpose up an octave:

```haskell
pat = p.mapP (+ 12) (p.fastcat [p.atom 60, p.atom 64])
```

```text
    0.000 on  ch1 72 100
 1000.000 on  ch1 76 100
```

## MIDI values

`funk-midi` and the CLAP plugin play a `Pattern Int`. Each value is:

```text
note + 128 * velocity + 16384 * (channel - 1)
```

A plain note number has velocity 0, which plays as 100, on channel 1. `m.vel` and `m.chan` set the other two fields:

```haskell
pat = m.chan 10 (m.vel 110 (p.euclid 3 8 36))
```

```text
    0.000 on  ch10 36 110
  750.000 on  ch10 36 110
 1500.000 on  ch10 36 110
```

| Function | Type | Effect |
|-|-|-|
| `m.vel v pat` | `Int -> Pattern Int -> Pattern Int` | velocity `v`, clamped to 1..127 |
| `m.chan c pat` | `Int -> Pattern Int -> Pattern Int` | channel `c`, clamped to 1..16 |

They clamp, not fail, so a typo cannot stop playback. `mapP (+ 12)` changes the note field only while the note stays within 0..127.

## Live coding

### funk-midi

Input is funk REPL input: declarations, expressions, and these commands:

| Command | Effect |
|-|-|
| `:l FILE` | load a file; if it defines `pat`, play it |
| `:cycle MS` | set the cycle's length from the next window; the beat continues |
| `:stop`, `:play` | silence and resume; the clock keeps running |
| `:wait MS` | play for `MS` ms; for scripts |
| `:q` | quit, after sending note-offs for every sounding note |

| Option | Effect |
|-|-|
| `FILE` | load `FILE` at start |
| `--script FILE` | run `FILE`'s lines as input, then exit |
| `--port N`, `--list` | send to output port `N`; list the ports |
| `--virtual NAME` | name the virtual port (default `funk-midi`) |
| `--cycle MS` | the cycle's length (default 2000) |
| `--print`, `--no-sleep` | print messages instead of sending them; run on a virtual clock |
| `-M DIR` | search `DIR` for modules |

Errors leave the music playing:

- **A definition that fails to compile, or fails as it is evaluated,** leaves the playing `pat`.
- **A `pat` that fails as it plays,** such as one that divides by zero, stops. It stays silent until `pat` is defined again.

### The CLAP plugin

`make clap` builds `build/clap/funk-midi.clap`. `make clap-dev` runs it without a DAW, sending MIDI to the first port or to a virtual port named `midi-langs-dev`.

In the editor, Ctrl+Return evaluates the paragraph around the caret, and Ctrl+. stops.

| Code | Live on | Live off |
|-|-|-|
| `pat = ...` | loops | becomes a one-cycle clip, which loops with the host |
| an expression of type `Pattern Int` | plays once, one cycle | becomes a one-cycle clip |
| anything else | evaluated; its value is printed | the same |

`sig = ...` plays a graph on the plugin's stereo output, as `funk-midi --audio` does, and every note the plugin sends also plays `voices f`, 8 voices of `f gate freq`:

```haskell
sig = voices (\gate freq -> d.adsr (d.dc 0.01) (d.dc 0.2) (d.dc 0.5) (d.dc 0.3) gate * d.saw freq)
```

The notes still go out as MIDI; route that output nowhere if the plugin's voices are the only synth. The graph needs a C compiler when the plugin loads; without one, MIDI still plays and the output is silent.

A cycle is 4 beats, one bar of 4/4, at the host's tempo. Stop interrupts a running evaluation. The heap is limited to 512 MiB and the stack to 256 MiB; past either, the evaluation fails with "out of memory" or "stack overflow", and the host goes on.

## Pitfalls

**A definition keeps the values it was compiled with.** Redefining a helper does not change a `pat` that uses it; evaluate `pat` again:

```haskell
bass = p.atom 36
pat = p.stack [bass]
bass = p.atom 40       -- pat still plays 36
pat = p.stack [bass]   -- now 40, from the next cycle
```

```text
    0.000 on  ch1 36 100
 2000.000 on  ch1 36 100
 4000.000 on  ch1 36 100
 6000.000 on  ch1 40 100
```

**A change sounds after the lookahead.** `funk-midi` schedules 50 ms ahead; the CLAP plugin, as far ahead as its host renders. An onset inside that window when you evaluate is played from the old pattern. A redefinition just at a cycle's start therefore sounds from the next cycle.

**A `String` is not a list.** `map p.atom "abc"` is a type error; `String` is packed. Use `unpack "abc"`, or a list of values.

**A field read needs a signature.** A helper such as `startOf e = e.whole.start` needs `startOf :: p.Event a -> Int`.

**A function that passes a function parameter to `mapP` cannot have a signature.** `up :: Int -> p.Pattern Int -> p.Pattern Int` with `up n pat = p.mapP (+ n) pat` compiles. `each :: (Int -> Int) -> p.Pattern Int -> p.Pattern Int` with `each f pat = p.mapP f pat` fails with "functions whose effects differ": the signature's `Int -> Int` cannot promise that `f` is pure, which `Pattern` needs ([dev/typed-effects.md](dev/typed-effects.md)). Leave that signature out; `mapP` itself has none for this reason.

## Using patterns from your own program

Below the combinators, `pattern` gives the pieces a scheduler needs.

| Function | Type | Gives |
|-|-|-|
| `p.query pat span` | `Pattern a -> Span -> [Event a]` | the events overlapping `span`, in ticks |
| `p.onsets pat span` | `Pattern a -> Span -> [Event a]` | the events that start in `span` |
| `p.sampleEvents pat cs a b` | `Pattern a -> Int -> Int -> Int -> [(Int, Int, a)]` | `(start, duration, value)` of the events starting in time units `[a, b)`, at `cs` units per cycle, in time order |
| `p.sampleOnsets pat cs a b` | `Pattern a -> Int -> Int -> Int -> [(Int, a)]` | the same without durations |

- **Events.** An `Event` has a `whole`, its full extent, and a `part`, the piece inside the query. `e.onset` is true where the two start together.
- **Units.** The time unit is up to the caller: samples, microseconds. A tick maps to `div (t * cs) p.cycleTicks`. Adjacent blocks `[a, b)`, `[b, c)` then split the onsets exactly: none lost, none repeated.
- **Values from C.**
  - **`p.writeEvents buf cap pat cs a b`** writes `sampleEvents` to a C array of `Int`s, three per event, and returns their number. A host calls it through `funk_call` in `libfunk.h`; see `bench/reload.c` and `projects/funk-midi/main.c`.
  - **`p.pushOnsets ring pat cs a b`** puts `sampleOnsets` on a `Ring Int`, for an audio-thread export ([dev/realtime.md](dev/realtime.md)). Each entry is one `Int`, `sample * 65536 + value`, so a push cannot tear. `p.eventSample` and `p.eventValue` decode it, without allocating, in the export. Values must be in 0..65535. `tests/ffi/sched.fk` is a complete example.
