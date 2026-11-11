# Patterns and MIDI

A guide to funk's music library: `lib/pattern.fk`, `lib/midi.fk`, and the two programs that play them. Design and measurements are in [dev/patterns.md](dev/patterns.md).

- **`pattern`** describes events over musical time. A pattern is a query: given a span of time, it returns the events in it. Patterns combine into larger ones.

- **`midi`** sets velocity and channel on a pattern of MIDI notes.

- **`funk-midi`** is a REPL that plays a pattern to a MIDI port and replaces it live when you redefine it.

- **The CLAP plugin** (`projects/funk-clap`) does the same inside a DAW, or in a standalone host.

The patterns drive an external synth over MIDI, or SuperDirt over OSC, as Tidal does (`funk-midi --dirt 57120`). `funk-midi --audio` also plays `sig`, a graph of `lib/dsp.fk`'s signals, to the audio device ([dev/audio.md](dev/audio.md), D).

Every output below comes from `funk-midi --no-sleep --cycle 2000`. Note-offs are left out.

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

The hosts import `lib/pattern.fk` both as `p` and unqualified, so `fastcat [atom 60, atom 64]` works too, as in Tidal. This guide writes `p.`, which a file needs unless it imports `pattern (..)` itself. A name you define at the prompt shadows an imported one, as at GHCi's prompt ([dev/modules.md](dev/modules.md)).

```text
    0.000 on  ch1 60 100
  666.666 on  ch1 64 100
 1333.333 on  ch1 67 100
```

`pat` plays from the next 10 ms window and repeats every cycle. Define it again to change what plays. `pattern` is imported as `p`, and `midi` as `m`.

## Time

- **A cycle** is the unit of musical time. A host gives it a length: in `funk-midi`, Tidal's 0.575 cycles a second by default, changed by `setcps` and an event's `cps`; 4 beats in the CLAP plugin.

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
| `p.fast k pat` | `Pattern Float -> Pattern a -> Pattern a` | `pat` `k` times per cycle, such as `1.5`; `0` is silence, a negative `k` reverses |
| `p.slow k pat` | `Pattern Float -> Pattern a -> Pattern a` | one cycle of `pat` over `k` cycles; as `fast` for `0` and negatives |
| `p._fastR n d pat`, `p._slowR n d pat` | `Int -> Int -> Pattern a -> Pattern a` | `fast` and `slow` by the ratio `n / d`, such as 3 and 2 for 1.5 |
| `p.rev pat` | `Pattern a -> Pattern a` | each cycle of `pat` backwards |
| `p.shift t pat` | `Pattern Int -> Pattern a -> Pattern a` | `pat`, `t` ticks later; negative is earlier |
| `p.euclid k n pat` | `Pattern Int -> Pattern Int -> Pattern a -> Pattern a` | `pat` on `k` of `n` steps, spread evenly, the first on step 0 |
| `p.mapP f pat` | | `pat` with `f` applied to each value |

A number is a pattern too, so `p.fast 2 pat` works, and so does a pattern of factors: `p.fast (p.fastcat [1, 2]) pat` plays the first half of each cycle at speed 1 and the second at speed 2. A `Float` variable needs `p.atom k`, or `p._fast k` and `p._slow k`, which take one; `p._shift n` takes an `Int` of ticks. A factor is taken as the nearest ratio with a denominator up to 65536, so `1.5` is 3/2 and `0.6` is 3/5, exactly. Patterns also add and multiply: `p.fastcat [60, 64] + 12`.

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

`p.euclid k n pat` puts `k` onsets on `n` steps, as evenly as possible:

```haskell
pat = p.euclid 3 8 36
```

```text
    0.000 on  ch1 36 100
  750.000 on  ch1 36 100
 1500.000 on  ch1 36 100
```

The onsets follow Bjorklund's algorithm, as Tidal's do: `p.euclid 5 8` plays `x.xx.xx.`. A negative `k` plays the other steps.

### Tidal's combinators

`lib/pattern.fk` has Tidal's transformations by their Tidal names: `every`, `whenmod`, `iter`, `ply`, `chunk`, `off`, `jux`, `sometimesBy`, `degradeBy`, `linger`, `struct`, `arp`, `rolled` and about 200 more, with the continuous `sine`, `saw`, `rand` and `perlin`. `lib/control.fk` has those that act on control patterns, such as `jux`, `chop`, `striate` and `stut`. Arguments follow Tidal's types, with one change: a time in cycles, or a speed factor such as `ply`'s or `segment`'s, is a `Float`, where Tidal's is a `Rational`.

```haskell
import pattern ((~>))
pat = p.every 3 (p.fast 2) (p.off 0.125 (\x -> x + 7) (p.mini "60 [63 67]"))
```

The list, and where funk differs from Tidal: [dev/combinators.md](dev/combinators.md).

### Changing values

`mapP` applies a function to every value; Tidal's `fmap` and `<$>` are the same, and `pure` is `atom`. Transpose up an octave:

```haskell
pat = p.mapP (+ 12) (p.fastcat [p.atom 60, p.atom 64])
```

```text
    0.000 on  ch1 72 100
 1000.000 on  ch1 76 100
```

### Mini-notation

`p.mini` parses Tidal's mini-notation into a `Pattern Int`:

```haskell
pat = p.mini "c4 [e4 g4] <c'maj e'min7> ~"
```

A number is a MIDI note. A note name is a letter `c` to `b`, then any of `s` (sharp), `f` (flat) or `n` (natural), then an octave: `c4` is 60, `fs3` is 54, and `e` is `e4`, 64.

| Syntax | Plays |
|-|-|
| `a b` | a sequence, in one cycle |
| `~`, `-` | a rest |
| `[a b]` | a sequence in one step |
| `a b . c d e` | feet of equal length: `[a b] [c d e]` |
| `<a b>` | one step per cycle, in turn |
| `{a b c, d e}` | steps at the first sequence's rate: `d e d` in the first cycle |
| `{a b c}%4` | 4 steps per cycle; `%` takes a pattern, as in `%<4 8>` |
| `a, b` | both at once, at the top or inside `[ ]`, `< >` and `{ }` |
| `a \| b` | one of them each cycle, at random; at the top or inside `[ ]` |
| `a*k`, `a/k` | `a` `k` times as fast or slow; `k` is a number such as `1.5` or `3%2`, or a pattern, as in `a*<2 1.5>`; a negative `k` reverses |
| `a!k`, `a !` | `k` copies of `a` in turn; `!` alone adds one |
| `a@k`, `a _` | `a` lasts `k` steps, a ratio such as `1.5` or `3%2`; each `_` adds one |
| `a(k,n)`, `a(k,n,r)` | `a` on `k` of `n` steps, as `p.euclid`, rotated left by `r` steps; each may be a pattern |
| `a?`, `a?0.3` | `a` dropped at random, half or 0.3 of the time |
| `a .. b` | the numbers from `a` to `b`, in one step |
| `c'maj`, `e4'min7'ii` | a chord, with Tidal's chord names and modifiers: `'i` inverts, `'o` opens, `'d1` drops, `'5` takes 5 notes |

Randomness is Tidal's: a value is a function of the event's start time and the `?` or `|` it comes from, so a pattern plays the same each time and in any block size. A typo or an unknown chord name fails when the definition is evaluated, so the playing pattern goes on. Tidal plays an unknown chord name as its root.

A string literal is mini-notation wherever a pattern is expected, as in Tidal: `p.fast "<2 4>" "bd sn"`, `p.every "<2 3>" p.rev "0 1 2"`, `"<0 0.25>" <~ pat`. It parses as Tidal's does for its type: `Pattern String` as `p.miniS`, `Pattern Float` as `p.miniF`, `Pattern Int` with Tidal's note names, `c5` being 0, and `Pattern Bool` from `t` and `1`, `f` and `0`, so `p.struct "t(3,8)"` and `p.mask "1 0 1 1"` work. A `Bool` euclid's off steps are `False`, as Tidal's are. `p.mini` keeps MIDI's, `c4` being 60, for `pat`'s notes. A literal whose type nothing fixes is a `String`, so `riff = "0 2"` needs a signature, `riff :: p.Pattern Int`, to be a pattern; `pat = p.fast 2 "0 2"` reports an ambiguous `Parseable a` until one fixes the element type.

`p.miniS` parses the same notation into a `Pattern String`, for names such as Tidal's samples: `p.miniS "bd*2 [hh sn:3] ~ cp?"`. A name starts with a letter or digit and goes on with letters, digits and `: . - _`; `bd .. sn` is `bd sn`, as in Tidal. `pat` plays a `Pattern Int`, so names become notes, in one of two ways:

- `m.drums` plays General MIDI drums on channel 10, for a drum instrument in the host: `pat = m.drums (p.miniS "bd*2 [hh sn:3]")`. `bd` is 36, `sn` 38, `hh` 42, `cp` 39, `oh` 46; an index picks a variant, so `bd:1` is 35. `m.gmDrums` lists the names. A name not in it is dropped.
- `d.kit` plays WAV files in the plugin, from a folder laid out as Tidal's Dirt-Samples, and `m.kit` sends the notes it plays. `scripts/dirt-samples.py` fetches Dirt-Samples to `~/.local/share/funk/Dirt-Samples`. `sn:3` is the 4th WAV file of `sn/`, sorted by name; the index wraps, as SuperDirt's does. The kit's names, in order, play on notes 0, 1, 2 and on:

```haskell
kit = ["bd", "sn:3", "hh"]
sig = d.kit "~/.local/share/funk/Dirt-Samples" kit
pat = m.kit kit (p.miniS "bd*2 [hh sn:3]")
```

A name not in the kit is dropped. A folder with no WAV file fails `sig`, and the last graph plays on.

`^name` reads a control set from outside, as `cF_` and its kin do for the pattern's type: `n "0 ^x"` plays control `x` as the second step. A factor cannot be `^name`, as in Tidal.

### Control patterns

`pat` may also be a control pattern, as Tidal's: events of named parameters, from `lib/control.fk`. `funk-midi` and the plugin import every name of it unqualified, as Tidal's are, with `import control (..)`:

```haskell
pat = note "0 [4 7]" # ccn "74" # ccv "<64 100>"
pat = n "0 12" # midichan "1" # gain "1.2" |+ n "<0 2>"
```

`funk-midi` also has Tidal's slots, from `lib/stream.fk`, imported as `import stream (..)`. Each plays at once, and each is replaced alone; `pat` of controls is `d1`'s. `d1` to `d12` play on orbits 0 to 11, and `_d1` or `d1_` silences slot 1 ([dev/stream.md](dev/stream.md)):

```haskell
d1 $ s "bd*2 sn"
p "hats" $ s "hh*4" # gain 0.8
```

Tidal's commands act on the slots: `hush`, `mute 2`, `solo "hats"`, `unmuteAll`, `list`, `once $ s "cp"`, `first`, `panic`, and `streamAll (# speed 2)` for Tidal's `all`. They also come as Tidal's OSC playback commands, `/mute` and the rest, on the `controls` ports ([dev/stream.md](dev/stream.md#commands)). Tidal's transitions replace a slot gradually: `xfade 1 $ s "hh*8"`, `jumpIn 1 2 $ ...`, `interpolate 1 $ ...`, `clutch`, `anticipate`, `mortal`, `forId` and the rest ([dev/stream.md](dev/stream.md#transitions)). Tidal's tempo commands set the clock: `setcps 0.6`, `setbpm 120`, `getcps`, `getnow`, `setCycle 0`, `resetCycles`, `nudgeAll 0.1` ([dev/stream.md](dev/stream.md#tempo)).

The controls are Tidal's, `gain`, `cutoff`, `legato` and 254 more, each taking a pattern of Tidal's type for it: `pan p.sine`, `n (p.irand 8)`, `sound (p.samples "bd*2 sn" (p.run 3))`, or a string literal of mini-notation. `n` and `note` take a `p.Note`, a `Float` whose mini-notation reads note names first, so `n "e"` is note 4, as in Tidal. A string literal as a whole control pattern is a `sound`, so `"bd*2 sn" # gain 0.8` works, and `drum "bd sn"` plays Tidal's General MIDI drum names as `n`. Tidal's `Simple.hs` shorthands, such as `louder`, `silent` and `faster`, are there too, and its scales: `n (scale "<major ritusen>" "0 .. 7")`, with `scaleList` naming all 76. Tidal's signals, such as `p.sine`, `p.rand` and `p.perlin`, have any `FromFloat` type, and `p.irand`, `p.run` and `p.scan` any `Num` type, so `n (p.segment 4 (p.range 0 7 p.sine))` is a pattern of notes. Patterns are `Fractional` when their values are, so `"1 2" / 4` and `(26/8) <~ p` work as in Tidal. The operators are Tidal's: `#` keeps the left's structure and takes the right's values; `|+|`, `|+` and `+|` add, with structure from both sides, the left or the right, and `||+` squeezes the right into each left event; `-`, `*`, `/`, `%`, `<` and `>` have the same forms. Tidal's `|>` is the prelude's `x |> f` here; `#` is the same operator. Notes are Tidal's: `note "0"` and `note "c5"` are middle C.

An event plays as SuperDirt's MIDI target plays it, if it has no `s` or `s` is `"midi"`:

| Control | MIDI |
|-|-|
| `note`, else `n`, plus `12 * octave` (5) | the note; `note "0"` is 60. `freq` overrides it |
| `amp` (0.4), `gain` (1), `overgain` (0) | velocity `amp * (min gain 2 + overgain)^4 * 127`, so 50 by default |
| `sustain`, else `legato` | the note's length: seconds, or a share of the event, less 1 ms |
| `midichan` | the channel, from 0 |
| `ccn` and `ccv`; `nrpn` and `val` | a CC; an NRPN's six CCs |
| `progNum`, `midibend`, `miditouch`, `polyTouch` | program change, pitch bend (0 to 16383), aftertouch, poly aftertouch in place of the note |
| `midicmd` | `start`, `stop`, `continue`, `midiClock`, `allNotesOff`, `control`, `songPtr`, and `bend` or `touch` by `val` |

Each message goes out at the event's onset, before its note.

The plugin's voices, and `funk-midi --audio`'s, read an event's other parameters through `voicesWith`, whose function also takes `param`: `param name default` is the voice's value of `name`, which its event sets as its note starts. A parameter MIDI takes, such as `note`, `gain` or `ccn`, is not sent to the voice.

```haskell
sig = voicesWith (\gate freq param -> d.lowpass1 (param "cutoff" 2000.0) (d.saw freq) * gate)
pat = note "0 4 7" # cutoff "<400 1200>"
```

An event whose `s` is a sample's name, not `"midi"`, plays that sample of `d.kit` as SuperDirt plays it: `begin`, `end`, `speed` (negative plays backwards), `loop`, `accelerate`, `sustain` or `legato`, `note` and `octave` for pitch, `unit "c"`, `cut` groups, `pan` (0 left, 1 right) and level, `amp / 0.4 * (min gain 2 + overgain)^4`, so 1 by default. So `chop`, `striate`, `slice` and `splice` play their parts. It sends no MIDI. `s "bd:3"` and `s "bd" # n 3` are the same sample:

```haskell
sig = d.kit "~/.local/share/funk/Dirt-Samples" ["bd", "bd:3", "sn"]
pat = s "bd*2 [sn bd:3]" # speed "<1 2>" # gain "0.9"
```

A name not in the kit is silent. Each name has 4 voices, so 4 of its events overlap; a fifth takes the oldest's voice. `d.kitWith n dir names` gives `n`, up to 16. Interpolation is linear, where SuperDirt's is cubic, and `unit "s"` plays as `unit "r"`.

The control names are taken at the top level of these sessions: `speed = 2` is an error, "`speed` is imported from `control`; choose another name". A parameter or `let` of that name is fine, and `control.speed` always means the control.

### Controls from outside

A top-level `controls` lists where values come from, and Tidal's `cF`, `cI`, `cS` and the rest read them, as do `in0` to `in127`:

```haskell
controls = [p.osc 6010, p.midiCC, p.params]
pat = p.segment 8 (p.range 48.0 72.0 (p.cF 0.0 "cc74")) -- a Pattern Float, mapped to notes as you need
```

- `p.osc port`: Tidal's OSC `/ctrl name value` on 127.0.0.1, under its own name. Tidal's port is 6010.
- `p.midiCC`: CC 74 as `"cc74"`, from any channel, and `"1:cc74"` from channel 1, 0 to 1. The plugin takes it from its MIDI input; `funk-midi` from `--in N` or `--in-virtual NAME`, and from `:cc CHANNEL NUM VALUE`.
- `p.params`: the plugin's parameters `"p1"` to `"p16"`, 0 to 1, which a DAW can automate.

In `funk-midi`, Tidal's `setF "x" "<0.25 0.5>"`, `setI`, `setS`, `setR` and `setB` set a value from code, as a pattern; `getState "x"` reads one ([dev/stream.md](dev/stream.md#state)).

`trigger` and `qtrigger` start a pattern from the cycle it was defined at, and `sec` and `msec` turn seconds into cycles, from the hosts' `_t_pattern` and `_cps` ([dev/controls-in.md](dev/controls-in.md)).

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
| `--cycle MS` | the cycle's length (default 1739.13, Tidal's 0.575 cycles a second) |
| `--print`, `--no-sleep` | print messages instead of sending them; run on a virtual clock |
| `-M DIR` | search `DIR` for modules |
| `--dirt [HOST:]PORT` | send `pat`, a control pattern, to SuperDirt as Tidal does, in place of MIDI; SuperDirt listens on 57120 ([dev/dirt.md](dev/dirt.md)) |

Errors leave the music playing:

- **A definition that fails to compile, or fails as it is evaluated,** leaves the playing `pat`.

- **A `pat` that fails as it plays,** such as one that divides by zero, stops. It stays silent until `pat` is defined again.

### The CLAP plugin

`make clap` builds `build/clap/funk-midi.clap`. `make clap-dev` runs it without a DAW: its audio plays on the default output device, and its MIDI goes to the first port or to a virtual port named `midi-langs-dev`. `build/clap_dev_host --meter PLUGIN` also prints the output's peak every second.

The editor opens with a `sig` and a `pat`: evaluate both paragraphs to hear `pat`'s notes on the plugin's voices. Notes the DAW sends to the plugin, from a keyboard or a MIDI clip, play the same voices.

In REAPER, Space types while the code field is active; click outside it, or in another window, for the transport.

In the editor, Ctrl+Return evaluates the paragraph around the caret, and Ctrl+. stops: running code, notes and `sig`, which stays silent until it is evaluated again. A host resetting the plugin leaves `sig` playing.

| Code | Live on | Live off |
|-|-|-|
| `pat = ...` | loops | becomes a one-cycle clip, which loops with the host |
| an expression of type `Pattern Int` | plays once, one cycle | becomes a one-cycle clip |
| anything else | evaluated; its value is printed | the same |

`sig = ...` plays a graph on the plugin's stereo output, as `funk-midi --audio` does, and every note the plugin sends also plays `voices f`, 8 voices of `f gate freq`:

```haskell
sig = voices (\gate freq -> d.adsr 0.01 0.2 0.5 0.3 gate * d.saw freq)
```

`voicesIx f` is `voices` with the voice's number, from 1, as `f`'s first argument, for a noise seed per voice: `voicesIx (\k gate freq -> d.adsr 0.001 0.1 0 0.1 gate * d.noise k)`.

Functions with many parameters take them by name, and the rest have defaults: `d.adsr .{ attack = 0.001 } gate`, `d.reverb .{ mix = 0.5 } x`. `d.adsr`, `d.adsrExp`, `d.compress`, `d.comb`, `d.allpass`, `d.chorus`, `d.flanger`, `d.reverb` and `d.zita` take them; see [dev/named-args.md](dev/named-args.md).

`d.zita x` is a stereo reverb, Zita-Rev1, for a mono or stereo `x`. `d.reverb` is Freeverb's structure, per channel; `d.zita` costs about 4 us per 64 frames and shares state across its channels. Its parameters can move while it plays: they glide over about 50 ms, and moving `predelay` bends the pitch of the tail's input. Its names and defaults are `predelay` 0.06 s, `crossover` 200 Hz, `damping` 6000 Hz, `lowDecay` 3 s, `midDecay` 2 s and `mix` 0.3.

A paragraph with a `foreign export` is a kernel: your own unit generator, compiled to machine code and loaded while the music plays, in about 90 ms. `g.Node` plays it, with its initial state, then its inputs:

```haskell
import ugen as u
foreign export ccall "my_fold" foldK :: Ptr () -> Ptr (Ptr CFloat) -> Ptr CFloat -> CInt -> Ptr u.Context ->> ()
foldK :: Ptr () -> Ptr (Ptr CFloat) -> Ptr CFloat -> CInt -> Ptr u.Context ->> ()
foldK st ins out n cx = go (u.input ins 0) out 0 (fromIntegral n)
go :: Ptr CFloat -> Ptr CFloat -> Int -> Int ->> ()
go x out i n = if i == n then () else do
  u.put out i (abs (u.at x i) * 2.0 - 1.0)
  go x out (i + 1) n

sig = voices (\gate freq -> gate * g.Node "my_fold" [] [d.saw freq] * 0.2)
```

Evaluate the kernel again after an edit: `sig` picks it up, and the nodes that use it restart, unless its code is unchanged. The newest kernel of a name wins, so `dsp_saw` can replace `d.saw`'s. A kernel that fails to compile leaves the old one playing. A kernel's paragraph has no blank lines and stands alone: it sees `ugen`, the prelude and what it imports, not your other definitions. It must not allocate, and it needs a C compiler on this machine (`$CC`, or `cc`); without one, only the built-in kernels play.

`d.sample path gate speed` plays a WAV file once at each rising gate, in two channels. `speed` 1 is its own pitch, so `freq * d.dc (1.0 / 261.63)` plays middle C at that pitch. `d.wavetable values freq` repeats one cycle of values, which `d.cycleOf n f` makes, and `d.wavetableFile path freq` reads them from a file:

```haskell
sig = voices (\gate freq -> d.sample "/abs/path/piano.wav" gate (freq * d.dc (1.0 / 261.63)))

sig = voices (\gate freq -> d.adsr 0.01 0.2 0.5 0.3 gate * d.wavetable (d.cycleOf 256 (\x -> if x < 0.5 then 1.0 else -1.0)) freq * 0.2)
```

Use an absolute path, or one from `~/`, your home: a plugin's working directory is its host's. A replaced file plays from the next `sig`.

`d.key k` is note `k`'s gate, on any channel, while it is held; `d.kit` plays each sample on its note's key, and `d.sampleOf dir name gate speed` plays one name as `d.sample` plays a file.

A channel's notes play `voicesOn c f` instead, for MIDI channel `c` from 1 to 16, if `sig` has it:

```haskell
sig = voices lead + voicesOn 10 drum
```

The notes still go out as MIDI; route that output nowhere if the plugin's voices are the only synth. The plugin needs no C compiler: its foreign imports' glue is linked in.

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

**A function given to `mapP` must be pure.** `p.mapP print pat` is an error: a pattern may be queried at any time, so its query cannot act ([dev/typed-effects.md](dev/typed-effects.md)). The same holds for a function that passes its parameter to `mapP`, such as `each :: (Int -> Int) -> p.Pattern Int -> p.Pattern Int`.

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
