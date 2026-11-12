# Changelog

Format: [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [Unreleased]

### Added

- Events as SuperDirt plays them, in funk-midi `--audio` and the plugin. A top-level `dirt` lists sounds and modules, with SuperDirt's names. Each event of one of its sounds plays on its own chain: the sound, then each module whose test passes, in order, on its orbit. A chain is made from templates in a fixed pool on the audio thread, so events of one name overlap without `d.kit`'s 4 voices (S10). An event starts in its 64-frame block, its output delayed to its frame, as scsynth's `OffsetOut`. Templates over per-voice copies of every module: a voice of each sound with every module would render while silent, and still limit a name's overlaps. Graphs are unchanged; `d.orbit` reads an orbit into one ([docs/dev/dirt-sound.md](docs/dev/dirt-sound.md)).

  ```haskell
  dirt = d.addModule "half" (\param x -> x * d.dc 0.5) (d.has "cutoff")
           (d.loadSoundFiles "~/.local/share/funk/Dirt-Samples" ["bd", "sn:3"] d.noDirt)
  d1 $ s "bd*8 sn:3" # cutoff "300"
  ```

- A kernel ends its event by setting `funk_context`'s new `done`, as SuperCollider's `doneAction: 14`. A template's node named `>NAME` sets the event's parameter NAME for the stages after it, so modules and `dirt_gate` read the sustain a sample's time span computes, as SuperDirt's read DirtEvent's.

- A sample event plays SuperDirt's `dirt_sample` and `dirt_gate`, ported from SuperCollider 3.13.0's UGens in float arithmetic: cubic `BufRd`, `Line.kr`, `LFSaw`, `Pan2` and `Balance2`, `EnvGen`'s `\sin` segments and `DirtGateCutGroup`. A sample's modules run before its envelope, as SuperDirt's. `lib/dirt-scsynth` renders 16 cases with SuperDirt's own SynthDefs and `DirtEvent` code in scsynth, and funk matches every frame within 1.8e-7; a frame's shift misses by 0.018 or more. It runs where `sclang` and the SuperDirt quark are installed. Pan is equal-power, so a centred sample is 3 dB down on each channel against `d.kit`'s.

- `d.superDirt`: SuperDirt's 12 core effect modules, in its order: `vowel`, `shape`, `hpf`, `bpf`, `crush`, `coarse`, `lpf`, `pshift`, `envelope`, `grenvelo`, `tremolo` and `phaser`. Each runs its SynthDef's UGen graph, ported from SuperCollider 3.13.0: `RLPF`, `RHPF`, `BPF`, `Latch`, `Impulse`, `SinOsc`, `LFPar`, `AllpassL`, `PitchShift`, `XFade2` and `EnvGen`'s curves. `lib/dirt-scsynth` adds 18 cases, run through SuperDirt's own module functions; all 34 match within 2.2e-7. Three behaviours of SuperDirt's that funk copies:
  - An argument a module names but the event lacks is 0, not the SynthDef's default: sclang sends nil as 0. `# attack 0.01` alone has release 0.
  - scsynth constructs a synth at its first calc, so a filter's constructor filters the chain's first frame once more.
  - On arm64, scsynth's audio-rate `/` is nova-simd's reciprocal estimate with one Newton step, good to about 16 bits. `shape` divides that way.

  Templates now share pools by size class, templates whose sizes round up to one power of two in one pool. A class has 256 slots, or as many as fit in 8 MB, scsynth's default real-time memory. A `pshift` stage, 196 KB at 44.1 kHz, gets 42. One pool sized by the largest template took 50 MB.

  ```haskell
  dirt = d.loadSoundFiles "~/.local/share/funk/Dirt-Samples" ["bd", "sn:3"] d.superDirt
  d1 $ s "bd*8 sn:3" # cutoff "800" # resonance "0.3" # vowel "a"
  ```

- SuperDirt's orbits and global effects: `d.superDirt` gives each of 16 orbits `dirt_delay`, `dirt_reverb`, `dirt_leslie` and `dirt_monitor`, run after the orbit's events in scsynth's node order. Each pauses when silent and resumes on an event with one of its controls, as `DirtPause` and `GlobalDirtEffect.set`; a redefined `dirt` keeps an unchanged effect, its tail included. The monitor limits each orbit at 1, so `dirt` plays 20 ms late, as SuperDirt does. The delay is sc3-plugins' `SwitchDelay`; `d.superDirtCore` has SuperDirt's `CombL` for a SuperDirt without sc3-plugins. The reverb draws from a port of scsynth's random generator, which `g.seed` seeds as `RandSeed`. `lib/dirt-scsynth` grows to 48 cases on 16 orbits, all within 1.8e-7 of scsynth. Template names `o.` and `a.` add an orbit's effects (`graph.h`).

  ```haskell
  d1 $ s "bd*4" # room "0.4" # size "0.6" # delay "0.5" # delaytime "0.125"
  ```

- SuperDirt's synths, from their SynthDefs: `tests/dirt/synthdefs.scd` writes each SynthDef's UGen graph, as scsynth runs it, to `lib/synthdefs.fk`, and `lib/scsynth.fk` builds it into a template, one `lib/kernels.fk` kernel per UGen. Each UGen kernel picks scsynth's calc function by its inputs' rates and is constructed as scsynth constructs a synth (`funk_graph_ugen` in `graph.h`). One UGen library serves every synth, so a synth needs no kernel of its own; `superfm`'s presets, which sclang draws at random when SuperDirt starts, come with its data. `d.superDirt` and `d.superDirtCore` play 5 of `default-synths.scd`'s 11: `imp`, `psin`, `gabor`, `dirac` and `cyclo`. `d.superDirt` also plays 28 of `default-synths-extra.scd`'s 32, which SuperDirt loads with sc3-plugins, from `supergong` to `superprimes` and `soskick`. `lib/dirt-scsynth` adds 37 synth cases, all within 1.2e-7 of scsynth and 21 exact; `lib/synthdefs` checks the data against the installed SuperDirt. Its UGens compute `a * b + c` unfused: probes of scsynth's `Pulse` show SuperCollider.app 3.13.0 does not fuse it, and `supertron`'s feedback grew one ulp to 1e-5.

  ```haskell
  d1 $ n "0 5 7" # s "imp" # sustain "0.2"
  ```

  A stage's kernel now frees the stage by setting `funk_context`'s new `freeself`, as `doneAction: 2` frees a synth while its `dirt_gate` plays on. A template's node named `fed:NAME` takes, at each block's end, the output of the node named `feed:NAME`, for `LocalIn` and `LocalOut`. SynthDefs whose constants sclang draws at random, `superhammond`'s phases and `superfm`'s presets, are drawn with sclang's generator seeded, so funk's and the comparison's are the same; a SuperDirt boot draws others.

### Changed

- A synth event's records carry what `DirtEvent` sends a synth: `pan` from -1 to 1, `n`, `speed` times `cps` for `unit "c"`, and `accelerate` only if given.

- `dirt` events play at SuperDirt's level, `amp` 0.4 times `(min gain 2 + overgain)^4`, computed in floats as `dirt_gate`'s SynthDef does: 8 dB below `d.kit`'s level 1. The monitor's limiter and softclip and its silence threshold act on absolute levels, so only that level matches SuperDirt.

- A synth event's `sustain` excludes its fades, and a given `fadeTime` sets them, as SuperDirt sends them.

### Fixed

- `kernels/templates/thread` failed about once in 16 runs. Each template set waited 3 audio calls, as few as 35 frames, so the next set's tones could overlap the last set's and pass the peak bound of 8 overlapping tones. It now waits at least 64 frames.

- `funk emit-c --kernels` emitted calls to `fk_s_div`, `fk_s_mod`, `fk_s_shl`, `fk_s_shr` and their kin without their definitions, which are in `rt.c`, not the kernels' `rt.h`. The check accepts them in a kernel, `div` and `mod` by a nonzero literal, so such a kernel passed the check and failed to compile.

- `d.kit`'s sample voice averaged an accelerating speed as `|speed| + |endSpeed| * 0.5`. sclang reads binary operators left to right, so SuperDirt's `speed.abs + endSpeed.abs * 0.5` is their mean, and a sample with `accelerate` ended early.

## [0.11.0] - 2026-11-11

### Added

- Haskell's `Enum` class in the prelude, with `succ`, `pred`, `toEnum`, `fromEnum` and ranges, for `Int`, `Float`, `Char`, `Bool`, `Ordering`, `()`, `Note` and `deriving Enum`. A `Float` range follows Haskell's rule, so `[1.0 .. 2.5]` is `[1.0, 2.0, 3.0]` and Tidal's `run 3.5` ends at 3. `Int` ranges still fuse into loops.

  ```haskell
  data Colour = Red | Green | Blue deriving (Show, Enum)
  main = print [Red .. Blue]
  ```

- Tidal's stateful controls in funk-midi: the generated `xTake`, `xCount` and `xCountTo` families, `pStateF` and `pStateList`, as Tidal's `Params.hs` has them. A counter keeps its value across windows and slots, in one map with the controls `setF` sets, as Tidal's does. `pB`, `pR`, `pX` and `array` make Bools, ratios and byte blobs, sent to SuperDirt as Tidal sends them.

  ```haskell
  d1 $ s "drum*8" # nCount "c"            -- n 0, 1, 2, ... across cycles
  d2 $ s "arpy*3" # speedTake "t" [1, 1.5, 2]
  ```

- Tidal 1.10's steps: each pattern knows its steps a cycle, as Tidal's does, and `stepcat`, `steptake`, `stepdrop`, `expand`, `contract`, `extend`, `stepalt`, `stepzip`, `stepJoin` and `pace` use them. A constant argument applies without a join, as Tidal's `pureValue` lets it, so an event crossing a cycle boundary is one event. `tests/tidal/Steps.hs` checks 147 cases against Tidal.

  ```haskell
  d1 $ s (stepcat ["bd sn", "hh*3"])  -- 5 steps a cycle: 2 then 3
  ```

- `^name` in mini-notation reads a control, as in Tidal: `n "0 ^x"` plays control `x`, set by `setF` or OSC, as its second step.

- Tidal's `drawLine`, `drawLineSz`, `draw` and `stepcount`, and `Char` mini-notation, a letter or digit a step. `drawLine "a*2 [b c]"` prints the cycles as text, as in Tidal.

  ```
  [15 cycles]
  |aabc|aabc|aabc|aabc|aabc|aabc|aabc|aabc|aabc|aabc|aabc|aabc|aabc|aabc|aabc
  ```

- Tidal's `Time.hs` and most of `Pattern.hs`: `sam`, `cyclePos`, `arcCycles`, `subArc`, `withQueryTime`, `withEvent`, `splitQueries`, `compressArc`, `collect`, `patternify`, `extractF` and about 70 more, for writing combinators. A time is a `Float` of cycles, as elsewhere, and a `Span`, Tidal's `Arc`, holds ticks; `toTime` and `fromTime` convert. `chordList` names the chords. `lib/tidal` checks 36 cases against Tidal ([docs/dev/tidal-comparison.md](docs/dev/tidal-comparison.md), F2, F3 and F7).

- Haskell's `Semigroup` in the prelude: `<>`, infixr 6, for `String` and lists. On patterns it overlays, as in Tidal: `sound ("bd sn:2" <> "cp*3")`. `Monoid` waits for constant class methods.

- A fixity declaration may name a class method of the same module, as in Haskell: `infixr 6 <>`. It was rejected as lacking a binding.

- Tidal's scales: `scale`, `scaleList`, `scaleTable` with its 76 scales, `getScale`, `scaleWith`, `scaleWithList`, `raiseDegree`, `lowerDegree`, `raiseDegrees` and `lowerDegrees`. `lib/tidal` checks every scale against Tidal.

  ```haskell
  d1 $ n (scale "<major ritusen>" "0 .. 7") # s "superpiano"
  ```

- `class (Fractional a, Ord a) => RealFrac a` in the prelude, with `floor`, `ceiling`, `round` and `truncate`, for `Float` and `Note`. `quantise`, `qfloor`, `qceiling` and `qround` take any `RealFrac`, as Tidal's do. The results are `Int`s, not Haskell's any `Integral`, since funk's `Integral` cannot make a value.

- Tidal's `Simple.hs` in `lib/control.fk`: `crunch`, `scratch`, `louder`, `quieter`, `silent`, `skip`, `left`, `right`, `higher`, `lower`, `faster` and `slower`. A string literal as a control pattern is a `sound`, as in Tidal: `euclidFull 3 7 "realclaps" ("realclaps" # gain 0.8)`.

- Tidal's `grain'`, `midinote`, `drum` and `drumN`: `drum "bd*2 [~ hh]"` plays General MIDI drums by Tidal's two-letter names.

- `chromaticiseBy` takes any `(Num a, Ord a)` pattern, as Tidal's does, so `up (chromaticiseBy "0 1" "0 3")` works on notes.

- `Bool` mini-notation, as Tidal's: `t` and `1` are `True`, `f` and `0` are `False`, and a euclid's off steps are `False`. `lib/tidal` checks 25 cases against Tidal.

  ```haskell
  d1 $ struct "t(3,8)" $ s "cp"
  d2 $ mask "1 0 1 1" $ s "hh*8"
  ```

- The test `lib/tidal-docs` plays the 248 `d1 $` examples of Tidal's doc comments that Tidal runs through `funk-midi --dirt` and Tidal's `toOSC`. 234 send what Tidal sends, and 13 are rejected ([docs/dev/doc-census.md](docs/dev/doc-census.md)).

- Speed factors are `Float`s, as Tidal's are `Rational`s: `fast`, `slow`, `ply`, `segment`, `hurry`, `loopAt`, `smash`, `fastGap`, `fastSqueeze` and the rest, and `_fast`, `_slow`, `_ply` and `_segment`. Each factor is its nearest ratio with a denominator up to 65536, so `slow "<1.5 0.75>"` is exact in ticks. A `Pattern Int` factor needs `toFloat <$>`, as Tidal's needs `fromIntegral <$>`. A negative `slow` now reverses, as Tidal's does, where it raised an error. `lib/tidal` checks 17 cases against Tidal ([docs/dev/tidal-comparison.md](docs/dev/tidal-comparison.md) T2).

  ```haskell
  pat = slow "<1.5 0.75>" $ s "bd*4"
  ```

- Tidal's `fmap`, `<$>`, `<$`, `pure`, `<*>`, `<*`, `*>`, `<<*`, `liftA2`, `return` and `>>=` in `lib/pattern.fk`, for patterns only, as funk has no higher-kinded classes. `lib/tidal` checks 12 cases against Tidal ([docs/dev/tidal-comparison.md](docs/dev/tidal-comparison.md) L3).

- `funk-midi` and the plugin import `lib/pattern.fk` unqualified as well as `p`, so Tidal's names work as written: `every 3 (fast 2) $ s "bd sn"`. A REPL or host input may define a value an import has, which then shadows it, as GHCi's prompt does; a file keeps the error. funk-midi's `sig` is also `pattern`'s, so the hosts need the shadowing ([docs/dev/modules.md](docs/dev/modules.md)).

- Tidal's `setF`, `setI`, `setS`, `setR`, `setB` and `getState` in `funk-midi`. `cF` and the rest read a pattern set by name, as Tidal's `VPattern`, until an OSC `/ctrl` sets that name. `setR` takes `Float`s. The test `lib/tidal` checks 9 cases against Tidal ([docs/dev/stream.md](docs/dev/stream.md#state)).

  ```haskell
  setF "lpf" "<500 2000>"
  d1 $ s "bd*4" # cutoff (cF 1000 "lpf")
  ```

- Tidal's transitions in `funk-midi`: `xfade`, `xfadeIn`, `jump`, `jumpIn`, `jumpIn'`, `jumpMod`, `jumpMod'`, `wait`, `waitT`, `mortal`, `interpolate`, `interpolateIn`, `clutch`, `clutchIn`, `anticipate`, `anticipateIn`, `histpan` and `forId`, with `Transition.hs`'s mappers for `waitT`. The test `lib/tidal-dirt` runs 12 through Tidal's own mappers, a window at a time; all 229 messages match ([docs/dev/stream.md](docs/dev/stream.md#transitions)).

  ```haskell
  d1 $ s "bd*2 sn"
  xfadeIn 1 8 $ s "hh*8"
  ```

- Tidal's tempo in `funk-midi`: `setcps`, `setbpm`, `getcps`, `getbpm`, `getnow`, `setCycle`, `resetCycles` and `nudgeAll`, and their OSC forms on the `controls` ports. An event's `cps` sets the tempo at its onset, as Tidal's `processCps` does, so `setcps` is Boot's `once . cps`. Tempos are clamped to Link's 20 to 999 bpm. The default is Tidal's clock's, 0.575 cycles a second, where it was 0.5; `--cycle 2000` keeps the old one. The getters return `Float`s, as funk has no `Rational` ([docs/dev/stream.md](docs/dev/stream.md#tempo)).

- Tidal's slot commands in `funk-midi`: `hush`, `mute`, `unmute`, `solo`, `unsolo`, `unmuteAll`, `unsoloAll`, `list`, `once`, `asap`, `first`, `panic` and `only`, as Boot.hs's, and `streamAll` for Tidal's `all`, whose name is the prelude's. Tidal's OSC playback commands, `/mute` to `/hush`, arrive on the `controls` ports. A window that fails returns each slot to its previous pattern, as Tidal's `doTick` does, where slots stopped until one was given. `only` takes `\_ -> action`, as funk is strict ([docs/dev/stream.md](docs/dev/stream.md#commands)).

- A REPL or host input that is an action, a function with an effect from `()`, is run, as GHCi runs an `IO` input, so `hush` alone hushes.

- Tidal's slots in `funk-midi`: `d1` to `d16` and `p "name"` each play a pattern, all at once, and replace it alone, as Tidal's `streamReplace` does. `_d1`, `d1_` and `_p` silence one. `d1` to `d12` add `orbit 0` to `11`, as `Boot.hs` does, so `pat` of controls, now `d1`'s, sends `orbit 0` to SuperDirt. The test `lib/tidal-dirt` plays 5 slots through Tidal's own `playStack`; all 40 messages match ([docs/dev/stream.md](docs/dev/stream.md)).

  ```haskell
  d1 $ s "bd*2 sn"
  p "hats" $ s "hh*4" # gain 0.8
  ```

- A top-level value may share an import's qualifier, so Tidal's `p` sits beside `import pattern as p`. `p.x` with no spaces stays qualified ([docs/dev/modules.md](docs/dev/modules.md)).

- `funk-midi --dirt [HOST:]PORT` sends `pat`, a control pattern, to SuperDirt as Tidal's `superdirtTarget` does: a `/dirt/play` bundle per onset with an `s`, its controls and Tidal's `cps`, `cycle`, `delta` and `_id_`, stamped at the onset plus its `nudge`. The test `lib/tidal-dirt` builds Tidal's own messages for 12 patterns through its `toOSC`; all 122 match funk's, float bits included. Bundles leave 0.1 s ahead, Tidal's margin, so `--dirt` raises the lookahead from 50 ms. It shakes hands as Tidal does, so it prints "Waiting for SuperDirt" until SuperDirt replies ([docs/dev/dirt.md](docs/dev/dirt.md)).

  ```sh
  funk-midi --dirt 57120
  pat = s "bd*2 [sn cp]" # n "<0 3>" # speed 2
  ```

- String literals are overloaded, as with Haskell's `OverloadedStrings`: a literal at a type with a `FromString` instance calls its `fromString`, and one that nothing fixes is a `String`, as a float literal defaults to `Float`. `lib/pattern.fk` parses a literal at `Pattern a` as Tidal's mini-notation for `a`, through `Parseable`, so Tidal's patterned arguments port as written. A `Pattern Int` literal reads note names as Tidal's `Int` does, `c5` being 0; `p.mini` keeps MIDI's. The overloading was rejected in [docs/dev/patterns.md](docs/dev/patterns.md#literals) for changing literal defaulting; the `String` default keeps it unchanged ([docs/dev/tidal-comparison.md](docs/dev/tidal-comparison.md) L1).

  ```haskell
  pat :: p.Pattern Int
  pat = p.every "<2 3>" p.rev (p.fast "<2 4>" "0 [4 7]")
  ```

- A `Fractional` class, with `/` and `recip`, as Haskell's. `/` was a `Float` primitive. `Pattern a`, `Note` and `Controls` are instances, as in Tidal, so `(26/8) <~ p` and `n ("0 7" / 2)` port as written. A float literal at a control pattern is a `speed`, as Tidal's `fromRational` makes it. `FromFloat` stays the class of float literals, as a superclass, and a function without a signature that divides is still `Float`, so unboxed code is unchanged. `fromRational` waits for a `Rational` type. `lib/control.fk`'s `Divide` class is gone; `|/|` and its forms take `Fractional` ([docs/dev/tidal-comparison.md](docs/dev/tidal-comparison.md) L4).

- Tidal's combinators, ported from tidal-core 1.10.2: all of `Core.hs`, `UI.hs` and `Control.hs` but what is commented out there and what needs the host's state, such as `cF` and `trigger`. `lib/pattern.fk` has about 260 functions, from `every`, `whenmod`, `iter`, `chunk`, `ply`, `off`, `struct`, `arp` and `rolled` to `ur` and `markovPat`; `lib/control.fk` has those on control patterns, such as `jux`, `chop`, `striate`, `splice`, `stut` and `fix`. Arguments keep Tidal's types, but a time in cycles is a `Float`, rounded to a tick, and a speed factor an `Int`. The test `lib/tidal` runs 178 cases through the installed Tidal too and requires the same events; all match ([docs/dev/combinators.md](docs/dev/combinators.md)).

  ```haskell
  import pattern ((~>))
  pat = p.every 3 (p.fast 2) (p.off 0.125 (\x -> x + 7) (p.mini "60 [63 67]"))
  ```

- Continuous patterns, as Tidal's: `sine`, `saw`, `tri`, `square`, the `env` shapes, `rand`, `irand`, `perlin` and `normal`. An event's `whole` is now a `Maybe Span`, `Nothing` for a continuous event, which never has an onset. A separate signal type was the alternative; Tidal's `fast 2 sine` and `0.25 ~> rand` would not then port as written. A float literal is an atom, as a number literal is.

- A query carries controls from outside the pattern, as Tidal's `State` does, so Tidal's `cF`, `cI`, `cS` and the rest, `in0`..`in127`, `sec`, `msec` and the `trigger` family port. `p.query pat span` queries with none, and `p.queryState` with some. The hosts give `_cps` and `_t_pattern`, the cycle `pat` last changed at, so `trigger` and `sec` play in them, and the sources a top-level `controls` lists: Tidal's OSC `/ctrl`, MIDI CC as `"cc74"` and `"1:cc74"`, and the plugin's 16 CLAP parameters, `"p1"` to `"p16"`. `funk-midi` takes MIDI in with `--in` and `:cc` ([docs/music.md](docs/music.md#controls-from-outside), design in [docs/dev/controls-in.md](docs/dev/controls-in.md)). Queries cost about 12% more in the interpreter.

- The test `lib/tidal-patches` ports the 22 patches of Tidal's `old/examples/cycles.tidal` and matches them against Tidal 1.10.2. All 6456 events match exactly. In the interpreter, the slowest 10 ms query took 2.0 ms ([docs/dev/patch-port.md](docs/dev/patch-port.md)). `examples/tidal` has 18 of them to play on `d.kit`. `scripts/dirt-samples.py` fetches Dirt-Samples at a pinned commit, since it has no license file to ship it under.

- Tidal's event contexts: each event holds the positions of the mini-notation atoms it came from, as `e.context`, combined by each function as Tidal's are. `setContext`, `withContext`, `deltaContext` and `deltaMini` are in `lib/pattern.fk`. `rot` now orders a cycle's events by context first, as Tidal's does, so `rot 1 (rev "a b c")` is `a c b`. Editor highlighting needs the positions too (R6).

### Changed

- A cycle is 46126080 ticks, not 720720: 64 times as many, so binary steps down to 1024ths are exact. All 6456 events of `lib/tidal-patches` now match Tidal exactly, where 1604 rounded by up to 30 ticks, and 235 of the doc census's examples, where 202 did. Code that writes ticks as literals, such as `p.Span 0 720720`, should use `p.cycleTicks`. Times in samples or microseconds now convert to ticks by quotient and remainder: the direct product would pass `Int`'s range after 55 hours at the new rate.

- `enumFromTo` and `enumFromThenTo` are `Enum`'s methods; a function using a range at a type variable needs `Enum a`, as `run` now does. Optimization reports name a method at a known type as `enumFromTo@Int`.

- `p.Pattern` takes a query, steps and a pure value, as Tidal's; `p.pattern f` makes a pattern of a query, as `p.Pattern f` did.

- `funk build` passes `-falign-functions=64` to the C compiler. After the prelude gained `Semigroup`, `fib`'s generated C was unchanged but ran 15% slower: its hot function had moved from offset 0x30 to 0x10 in a 64-byte block. Aligned, the suite is at parity, and `__text` grows by about 15%, 7.7 KB for `pipeline`.

- A REPL or host input generalizes its value bindings, as GHCi's prompt does: `riff = "0 2"` is `FromString a => a`, so `n riff` and `s riff` both play in later inputs, and `k = 1` serves `k + 0.5`. Each value is computed once per type it is used at, and once at its default types as it is defined, so an error still shows at definition and the old pattern keeps playing. A file keeps the monomorphism restriction. `:t` prints the generalized type: `:t (/)` is `Fractional a => a -> a -> a` ([docs/dev/design.md](docs/dev/design.md#monomorphism-restriction)).

- A top-level function of float or string literals generalizes, as in Haskell: `half x = x * 0.5` is `(Num a, FromFloat a) => a -> a`, not `Float -> Float`, so it takes a `Sig` or a pattern without a signature. The literals defaulted first so that `Float` code would stay unboxed; specialization already does that, and all 25 benchmarks emit the same C.

- Controls take patterns of Tidal's types, not strings: `pan p.sine`, `n (p.irand 8)` and `sound (p.samples "bd*2" (p.run 8))` port as written, and a literal is still mini-notation. `n`, `note`, `up` and `number` take `p.Note`, Tidal's `Note`: a `Float` whose mini-notation reads note names first, so `n "e"` is note 4. Tidal's signals, from `p.sine` to `p.rand` and `p.perlin`, have any `FromFloat` type, and `p.irand`, `p.run` and `p.scan` any `Num` type, as Tidal's are, so they serve `n` too. A control given a `String` variable needs `p.miniS` for it; `pF` and its kind still take a pattern of one name ([docs/dev/tidal-comparison.md](docs/dev/tidal-comparison.md) L2).

- `d.kit` plays a sample event as SuperDirt does: `begin`, `end`, a negative `speed` backwards, `loop`, `accelerate`, `sustain` or `legato`, `note` and `octave` for pitch, `unit "c"`, `cut` groups, and its fades. Each name has 4 voices, so its events overlap, as SuperDirt's synths do; `d.kitWith n` sets the number. It played one event of a name at a time, from the start of its file. Interpolation is linear, where SuperDirt's is cubic.

- `p.euclid` takes Tidal's arguments: patterns of hits and steps, and a pattern to play. `p.euclid 3 8 36` still works; `p.euclid 3 8 'x'` needs `p.atom 'x'`.

- `p.fastcat []` is silence, as Tidal's is. It failed, as `fast 0`.

- `tools/` is `scripts/`, and `linux-sysroot.sh` is `linux-sysroot.py`. CMake builds `funkdsp.so` as a library target from `funk emit-c --kernels`'s C, with the compiler and flags of the build, instead of running `kernels.sh`.

- A sample folder or file path may start with `~/`, for `$HOME`: `d.kit "~/.local/share/funk/Dirt-Samples" ["bd"]`. The plugin's working directory is its host's, so a relative path never worked there.

### Fixed

- A chord's root in the mini-notation can be a whole term, as in Tidal: `"<c e>'maj"`, `"[c e]'min"`, `"c(3,8)'maj"`, `"c*2'maj"`. funk took the root only from a single note, so `"<c e>'maj"` was two steps, the second a chord on 0.

- A mini-notation range of strings or Bools, `"a .. c"` or `"t .. f"`, was `fastcat` of its two ends, where Tidal's `fromTo` plays each pair of their values. `"[a b] .. c"` played `a b c`; Tidal's plays `a c`.

- An instance of a class named with its module's qualifier, `instance p.Parseable Controls`, was rejected: "module `pattern` does not export type `Parseable`". The class applied to the head is kept as a type, and qualify resolved its name as one. `tests/run/modinstance.fk` covers it.

- A function whose signature has `->>` could not call a pure parameter inside an effectful lambda: `g f = mapM_ (\x -> print (f x)) xs` was rejected. The call through `f`'s signature `->` bound the lambda's still unknown effect to it, so the later `print` failed; it is now checked once the lambda's effect is known.

- `FUNK_GCSTRESS=1` failed funk-midi's check of marks, on HEAD before Tidal's slots and on every slot script after. The collector was correct: Clang's `___chkstk_darwin`, called before an interpreter VLA, saved a scratch register holding `fill_closure`'s one-past-the-end pointer into the new frame, and the check read it as a root of the next object. Under stress the VLAs start zeroed, as the dead stack already does ([docs/dev/261110-gcstress-chkstk.md](docs/dev/261110-gcstress-chkstk.md)).

- In the REPL and hosts, redefining a name could keep the old definition, or reach code defined before it, depending on the copies the compiler had made:
  - After `f x = x + 1`, `f 2`, `f x = x + 10`, `f 2` gave 3. The copy `f@Int` was found by name, and the name outlived the definition. A copy of a function applied to `(+ root)`, as in `map (+ root) xs`, and element reuse's copies had the same fault.
  - After `g y = f y`, a new `f` changed `g 2` when `g` was overloaded, but not when it was monomorphic: a copy made later named the new `f`. Code now keeps the definitions it was written against, as in GHCi and so in Tidal; re-evaluate it to use a new one.

- A mini-notation step longer than a cycle, such as `[lt*2]/5` in `"[sid/4]*3 [lt*2]/5 mt*3"`, had a different whole from Tidal's, so a join over it, as in `spread'`, gained or lost onsets. funk built an equal-weight sequence with `fastcat`, where Tidal's parser uses `timeCat`. funk's `timecat` also mapped a whole from the query's cycle, where Tidal's `_fastGap` maps it from its own. `rev "60*1.5 [61 62]*<2.5 3>"` has 130 onsets in 20 cycles, as in Tidal, not 140.

## [0.10.0] - 2026-11-08

### Added

- CI uploads the plugin for macOS, for Linux and cross-built for Linux with `zig cc`, and `funk` with `lib/` for macOS and Linux, as one artifact, `funk-builds`, once every plugin has passed its run. Each build is a tarball, since an artifact's zip drops the executable bits.

- `import m (..)` brings in every name `m` exports, unqualified, each type and class with its members. The names meet an import list's checks: another import of the same name, or a top-level definition of it, is an error naming the module, and a local binding shadows it. For a module written to be opened, such as `control`; others stay qualified, so that a name added to them cannot fail an importer's compile ([docs/dev/modules.md](docs/dev/modules.md#imports)).

- `lib/control.fk` builds Tidal's control patterns: events of named parameters, with Tidal's 257 controls, generated from its `Params.hs`, and its operators, `#` and the `|+|`, `|+`, `+|` and `||+` forms of `+ - * / % ** ++ < >`. Values and arithmetic follow Tidal's, quirks included; note names do too, `c5` being middle C, so ported patches keep their pitch. `pat` may be one: `funk-midi` and the plugin play it as SuperDirt's MIDI target does, as notes, CC, NRPN, program change, bend, aftertouch and clock, and on the voices. `voicesWith` gives a voice its event's other parameters, such as `cutoff`, and an event with a sample's `s` plays `d.kit`'s sample with its `speed`, `pan` and `gain` ([docs/music.md](docs/music.md#control-patterns), design in [docs/dev/delivery.md](docs/dev/delivery.md)).

  ```haskell
  import control (..)
  pat = s "bd*2 [sn cp]" # n "<0 3>" |+ speed "1 2"
  ```

- Sample names play. `m.drums` maps them to General MIDI drums on channel 10, for a drum instrument in the host. `d.kit dir names` plays WAV files in the plugin, from a folder laid out as Tidal's Dirt-Samples, and `m.kit names` sends the notes it plays: `sn:3` is the 4th WAV file of `sn/`, sorted, wrapping as SuperDirt's index does. Each name gets a note rather than its own control so that names travel over the `Int` notes the hosts already deliver ([docs/music.md](docs/music.md#mini-notation)).

  ```haskell
  kit = ["bd", "sn:3", "hh"]
  sig = d.kit "/abs/path/Dirt-Samples" kit
  pat = m.kit kit (p.miniS "bd*2 [hh sn:3]")
  ```

- `d.key k`, note `k`'s gate on any channel. `funk_voices` sends `kN.gate` while any note N is held, alongside the voices, which a note takes in turn and so cannot tell a kit which note it holds. `funk_sample_path` in `graph.h` finds a Tidal sample's file.

- `p.mini` parses Tidal's mini-notation into a `Pattern Int`, following Tidal's parser: sequences, rests, `[ ]`, `< >`, `{ }` and `%`, `.`, `,`, `|`, `*`, `/`, `!`, `@`, `_`, `(k,n,r)`, `?`, `..`, note names (`c4` is 60) and chords with Tidal's names and modifiers (`"e'min7'ii"`). Arguments may be patterns, as in `"60*<2 3>"`. `?` and `|` give Tidal's random values exactly, so a ported pattern drops the same events. Factors may be ratios, as in `"60*1.5"`. Parsing happens when `mini` is called, so a typo fails as the definition is evaluated and the playing pattern goes on ([docs/music.md](docs/music.md#mini-notation)). `p.miniS` parses the same notation into a `Pattern String`, for sample names such as `"bd*2 sn:3"`.

  ```haskell
  pat = p.mini "c4 [e4 g4]? <c'maj e'min7>(3,8) {60 62 64}%4"
  ```

- `lib/pattern.fk` applies and joins patterns, as Tidal does: `apP`, `apLeft` and `apRight` for `<*>`, `<*` and `*>`, and `joinP`, `innerJoin`, `outerJoin` and `squeezeJoin`. They are functions, not operators, so `<*` and `*>` stay free for a later `Applicative` ([docs/dev/patterns.md](docs/dev/patterns.md#implementation)).

- The CLAP plugin has a simulator mode, for the 0.10 acceptance test. With `FUNK_MIDI_CLAP_SIM` naming a script, each instance evaluates the script's steps live, in turn or at random, one per interval. It appends each step, the plugin's output and `process()`'s time against each block's duration to a log ([docs/dev/simulator.md](docs/dev/simulator.md)). Driving the plugin from inside, over UDP from a script outside, keeps the run in one process and lets the log place block timings beside the step that caused them.

- `funk emit-c --opt-report FILE`, and `funk_opt_report` in `libfunk.h`, list which optimizations applied to FILE's functions. Each pipeline fusion took is listed, as is each it refused, with the rule that refused it. Each emitted worker is listed with its parameter and result representations, and whether it loops, uses TRMC (tail recursion modulo cons) or reuses cells. A small change elsewhere can lose any of these with no other sign: `queens` lost 18.7% during 0.5 and 0.6. Diff the report across a change to see it.

  ```text
  queens.fk:10: not fused: filter, map: filter's function may act, or is not a known function
  queens.fk:10: fused: concatMap (enumFromTo, filter, map)
  queens.fk:10: queens@Int,Int/go12/$f8313@reuse1: (Int, boxed, boxed, Int, Int) -> boxed, loop, TRMC, reuses cells
  ```

- CI runs the suite with funk built under ThreadSanitizer, on Linux and macOS. 15 tests are disabled there: TSan's runtime faults past its fixed shadow call stack, at about 30,000 interpreter levels, before funk's stack overflows, and its shadow memory exceeds `embed/memory`'s limits. The suite raised no TSan report.

### Changed

- `p.euclid` follows Bjorklund's algorithm, as Tidal's does: `p.euclid 5 8` plays `x.xx.xx.`, not `x.x.xx.x`. Ported patterns now keep their rhythm. A negative `k` plays the other steps.

- `fast`, `slow` and `shift` in `lib/pattern.fk` take a `Pattern Int`, as Tidal's do: `p.fast (p.fastcat [1, 2]) pat`. A literal still works, since `Num (Pattern a)` makes it an `atom`; an `Int` variable needs `p.atom n`, or `_fast`, `_slow` and `_shift`, which take an `Int`. As in Tidal, a factor of 0 is silence, so `p.fast 0` no longer fails when defined, and a negative factor reverses, through the new `p.rev`. `_fastR n d` and `_slowR n d` take a ratio; they compose `_fast` and `_slow`, so time stays in integer ticks and rounds as `_fast 17` does.

- The plugin plays an expression once only if its own type is a `Pattern`. It used to try each expression as `Pattern Int`, which `Num (Pattern a)` would make `2 + 3` pass, playing note 5.

- The collector paces its cycles. It now starts a cycle early by the previous cycle's allocation while marking, and counts the objects marked during a cycle toward the next threshold. The heap therefore settles near the live data plus the threshold. Before, it settled near that plus twice a cycle's allocation. `churn` at 4,000,000 live cells: 287 MiB resident, against 333, and 13% slower from 3 more cycles. The benchmark suite is unchanged (+0.0%). Shortening mark steps instead was the alternative. It bounds the overshoot within a cycle only, which measured 24% of the live data at every size ([docs/dev/realtime.md](docs/dev/realtime.md#pacer)).

- The plugin's sessions collect after 16 MiB, not 128 MiB, since each instance has a heap. Over 7 minutes of playback, a session's resident set fell from 119 to 43 MiB, with the same time per window.

### Fixed

- The tree-sitter grammar, and with it the plugin editor's colours, parses named arguments, a signature's `x:T = default` and `f .{ x = 1 }`, and `import m (..)`. 16 of the repository's files failed to parse; none does now.

- The plugin's `pat` stopped for about 250 ms whenever kernels loaded, on macOS. The first `dlopen` of a new library blocks while the system checks it, and it ran on funk's thread, which queries `pat`. It now runs on the build's thread. `hosts/clap` failed on this; the simulator's log did not show it, since it times `process()` only.

- Cross builds. Build steps ran the `funk` and `embed` just built for the target, and `kernels.sh` compiled `funkdsp.so` with build-time `$CC` rather than the configured compiler. A cross build now runs a native build's tools, named by `FUNK_HOST_TOOLS`. libfunk links `-lunwind` when `_Unwind_Backtrace` needs it, and `-Wl,--exclude-libs,ALL` is passed only where the linker accepts it; `zig cc` needs both. The Linux plugin now builds on macOS with `zig cc` and `tools/zig-linux.cmake`, and CI runs the result on Linux ([docs/dev/cross-build.md](docs/dev/cross-build.md)).

- `ffi/sched/tsan-compiled` hung under load: its host played a fixed 600 blocks, and over 300 underruns ended them before musical time reached the end, so the scheduler waited forever. At a load average of 43, 23 of 24 runs hung. The host now plays until funk stops it, and 24 of 24 passed under the same load.

- An interpreter stack check could report "stack overflow" at once under GCC's AddressSanitizer, as `embed/plugin` did once in CI. The check compared a local's address against the stack's limit. On Linux, ASan's use-after-return check is on by default and moves such a local to a separate mapping, which may lie below the stack. The check now reads the stack pointer, as `fk_here` does. Confirmed with GCC 16's ASan on macOS, where the local lay 1.76 GB below the stack pointer. The CI failure itself was not reproduced.

- In REAPER, Space typed in the plugin's editor toggled the transport instead. The plugin now reports its active code field as a text field through REAPER's `hwnd_info` hook, since CLAP has no call offering a plugin a key ([docs/dev/plugin.md](docs/dev/plugin.md#keys-in-reaper)).

- A stack overflow in C called from a session, such as a recursive foreign import, is the runtime error "stack overflow", and the host goes on. It ended the process, with no call to `funk_on_fatal`'s hook, even from interpreted code. The handler resumes on the session's stack, over `siglongjmp`, which would leave the signal mask and signal stack to restore. An overflow inside a system library, an image that defines `malloc`, or the collector still exits, since a lock may be held, and now runs the hook, for at most 5 seconds ([docs/dev/embedding.md](docs/dev/embedding.md)). gh:2.

## [0.9.0] - 2026-11-01

### Added

- The plugin compiles a kernel while playing: a paragraph with a `foreign export` becomes a library in about 90 ms, and 9 ms when unchanged, and `sig` plays it from the next block. Over `dlclose`, old libraries stay loaded until the plugin goes, since a retiring graph may still run them. The compile has its own thread and session, because a session that installs exports refuses them, and a compile on the evaluator's thread made a looping `pat`'s notes 21 ms late. `funk_compile_kernels` is the API, and `lib/ugen.fk` holds what a kernel needs. It needs `$CC`; without one, the built-in kernels still play ([docs/music.md](docs/music.md)).

- A graph builds equal subtrees once, since equal kernels, states and inputs give equal samples. A signal used several times was built once per use: `d.reverb` on 16 voices made 2,628 nodes and now makes 260, and takes 21 us per block where it took 139. `d.chorus` and `d.flanger` gain too. Named nodes never merge. Across a live edit, a merge keeps the first node's state, and an unnamed node split from a shared one restarts: name both to keep their phases. A hash trie finds equal nodes, since a list scan took 152 ms to build 64 voices, against 12 ms now and 7 ms with no sharing.

- `d.zita`'s parameters glide over about 50 ms, so moving them does not click. The second difference after a jump fell 137x for predelay, and 6x to 136x for the decays. Targets are smoothed per block, and coefficients ramp per frame: retuning 8 lines per frame was the alternative, and costs 8 `exp` pairs each frame. Predelay changes pitch while it glides. Unmoving parameters match Faust as before, at 2% more cost.

- `d.zita` is Zita-Rev1, a stereo reverb ported from Faust's `re.zita_rev1_stereo`, with a mix added. At 48 kHz over 4 s, its output is within 1.4e-16 of Faust's, compared in double precision, which `tests/kernels/zita.c` keeps checked at 7 decimals. Its channels share state with no second kernel output: two nodes with the same inputs evolve identically, and each emits one channel. `g.Shared` gives both nodes every channel of the input. Splitting a stereo input by channel was the alternative: the graph has no shared subtrees, so that would copy the input's nodes.

  ```haskell
  sig = d.zita .{ midDecay = 4, mix = 0.4 } (voices (\gate freq -> gate * d.saw freq * 0.2))
  ```

- A signature can name parameters and give them defaults, and `.{ }` fills them by name. Positional calls and types do not change, so currying and existing code are unaffected. `lib/dsp.fk`'s 8 functions of four or more parameters have names and defaults. Values in `.{ }` and defaults must be pure. `:t` on the name shows them. `:t` now omits type qualifiers, for short lines, and the new `:tq` keeps them, since two modules' types of one name then print alike. Names are resolved at compile time, so a function passed as a value loses them; labels in types would keep them, but affect unification ([docs/dev/named-args.md](docs/dev/named-args.md)).

  ```haskell
  adsr :: attack:Sig = 0.01 -> decay:Sig = 0.2 -> sustain:Sig = 0.5 -> release:Sig = 0.3 -> gate:Sig -> Sig

  sig = voices (\gate freq -> d.adsr .{ attack = 0.001 } gate * d.saw freq * 0.2)
  ```

- `lib/dsp.fk` reads tables: `wavetable` and `wavetableFile`, an oscillator over one cycle, and `sample`, a WAV file played once at each rising gate. Tables are shared by file or content and freed with the last graph that reads them. A patch names a table and never holds its pointer, so it cannot write to it, and funk needs no finalizers ([docs/dev/dsp.md](docs/dev/dsp.md#decisions)).

- `d.polyIx` and the hosts' `voicesIx` pass `f` the voice's number, so each voice can seed its own noise. A variant over changing `poly`, whose patches stay valid.

- A field read on a parameter needs no signature when one loaded type has the field: `begin e = e.whole.start`. With several such types it is still an error, so loading another module can turn this code into an error, never into a different type.

- CI builds the CLAP plugin and runs its tests on Linux and macOS.

- Generated kernels use fused multiply-add where the target has it: a voice is 8% faster and `dsp_lowpass` 22%, on an M1. Other generated C stays uncontracted. Contraction over an explicit `fma`, which would need every kernel rewritten ([docs/dev/dsp.md](docs/dev/dsp.md#decisions)).

- A float literal can now be any `FromFloat` type: `Float` by default, `CFloat`, or `g.Sig`, so `d.saw 440.5` needs no `d.dc`. A literal in a top-level function that nothing else fixes is `Float`, so `f x = x * 0.5` stays `Float -> Float` and existing programs compile to the same code. Haskell's `Fractional` would have made `f` overloaded and passed a dictionary at run time. A function meant to take a signal without using one needs a signature ([docs/dev/type-classes.md](docs/dev/type-classes.md)).

- `lib/dsp.fk` gains delay effects: `delay`, `comb`, `allpass`, `chorus`, `flanger` and `reverb`, on delay memory from `funk_graph_memory`. A graph swap that keeps a node exchanges its buffer by pointer on the audio thread; copying, as other state does, would cost time in proportion to the delay. `reverb` runs Freeverb's structure once per channel, tuned apart, over one kernel with several outputs, which would change every kernel's signature ([docs/dev/dsp.md](docs/dev/dsp.md#phase-2-delay-memory)).

- `lib/dsp.fk` gains four kernels: `noise`, `saturate` (`tanh`), `compress` with `limit`, and `adsrExp`. `limit` is `compress` at an infinite ratio and instant attack, and holds its ceiling with no lookahead by distorting each peak within its frame; lookahead needs delay memory ([docs/dev/dsp.md](docs/dev/dsp.md)).

- The CLAP plugin's editor colours funk's syntax, from `projects/tree-sitter-funk`'s parser and highlight query, as Helix and Neovim do. ImGui's text field cannot colour its text, so the field draws it transparent and the plugin draws it over in colour, repeating ImGui's layout; a test compares every glyph with ImGui's own. A code-editor widget was the alternative, without the field's word wrap. Built when the generated parser and the tree-sitter runtime are present; otherwise the text is plain ([docs/dev/plugin.md](docs/dev/plugin.md#syntax-colours)).

- `projects/tree-sitter-funk`: a tree-sitter grammar for highlighting funk in Helix and Neovim, forked from tree-sitter-haskell 0.24.1, whose layout scanner handled funk's layout in every file. The fork adds funk's syntax: lowercase module names and qualifiers, definitions in a type's namespace, `foreign library`, `foreign struct` and `->>`. `make tree-sitter` parses every `.fk` file in the repository, 238, without errors; upstream parsed 166 of them. A new grammar would have needed its own layout scanner ([projects/tree-sitter-funk/README.md](projects/tree-sitter-funk/README.md)).

### Changed

- `make install` copies only the `funk` binary to `~/.local/bin`, and `PREFIX` is gone. To install `libfunk` and its CMake package, or to choose a prefix, use `cmake --install build --prefix DIR`.

- `pipeline` runs interpreted at n = 1,000,000 instead of 100,000,000 (`INTERP_SIZES` in `bench/bench.py`), which takes `make bench` from about 30 minutes to about 3. Interpreted time per element is unchanged; the larger size, chosen so C's vectorized loop is measurable, broke the benchmarks' 3 s rule.

- `make clap-dev` prints the plugin's audio peak each second (`--meter`), and the editor's default code says to evaluate `sig`, the instrument, before a pattern: the plugin is silent until both are evaluated.

- Unnamed graph nodes keep their state across `play`, as named ones do, so editing a graph without names no longer restarts its oscillators and clicks. `funk_player_play` names each by its shape, a hash of its kernel and its inputs, without initial states, and its order among nodes of that shape. A changed constant or a filter put around an oscillator keeps its phase; a changed kernel restarts the nodes above it. A node's path in the tree was the alternative; wrapping a signal or adding a term to a sum changes every path below. Playing silence first gives fresh states ([docs/dev/audio.md](docs/dev/audio.md#d-a-graph-of-compiled-unit-generators-built-at-run-time), item 4).

### Fixed

- The plugin's Stop silences `sig`. It released notes only, so a sample, a reverb's tail or a drone played on. A host's `reset`, which shared its code, still leaves `sig` playing: a host may reset at any time, and the plugin would stay silent until `sig` is evaluated again.

- Compiling a recursive function that passes itself a growing lambda, such as `nest n f = nest (n - 1) (\x -> f (x + 1))`, no longer takes over 20 GiB. Each specialized copy specialized its own recursive call again; the size limit measured each argument, which stayed short, while the chain of copies grew. The limit now covers the copy's whole name. One `compile_chunk` may also allocate at most 2 GiB (`FK_COMPILE_LIMIT`, in bytes), then the compile fails with an error and the session continues: four such compiles under `make test` ran a 16 GiB machine out of memory, faster than a test timeout could stop them.

- A signature may now take a function its body keeps in a pure data field: `mapP :: (a -> b) -> Pattern a -> Pattern b` was rejected with "functions whose effects differ", because a signature's `->` arrows share an effect that callers choose. Such a body now makes those arrows pure, so passing an effectful function is an error at the call. A marker for pure arguments was the alternative; it would add syntax for what the body already shows.

- In the CLAP plugin, a live evaluation that runs longer than the 50 ms lookahead plays its notes whole, starting when it ends. Its start time was taken before compiling, so its first note-on reached the audio thread late and played at a block's start, while its note-off stayed on time: in `tests/hosts/clap`, a 0.4 s evaluation's note lasted zero frames. `midi_clock_catch_up` moves script time to where a message still plays on time. Dropping the notes already late was the alternative; it loses a pattern's first note. (gh:5)

## [0.8.0] - 2026-10-30

### Changed

- The CLAP plugin allows any number of instances in one process, each with its own funk session, player and voices; it refused a second ([docs/dev/plugin.md](docs/dev/plugin.md#instances)). Its framework's audio calls are `Language` methods, since free functions could not tell instances apart.

- `libfunk.h` takes a session, `funk *`, on every call except `funk_version` ([docs/dev/embedding.md](docs/dev/embedding.md#phase-2-sessions)). `funk_new` starts the session's thread and compiles the prelude; `funk_run` runs a host's function there, queued if it comes from another thread; `funk_free` stops it. They replace `funk_main` and `funk_init`, and `funk_config` replaces `funk_memory`. Other calls made off the session's thread fail with a message. A process may run up to 256 sessions at once, each with its own heap, compiler and thread; they share nothing but the process. Code finds its session from the stack, which lies in an 8 GiB slot of address space per session, none of it committed: as fast as a global (interpreter +0.4%, compiled programs unchanged), where a thread-local cost the interpreter 27% ([docs/dev/261030-session-measurements.md](docs/dev/261030-session-measurements.md)). Output callbacks are a session's, so a failed `funk_new` reports on stderr; a `StablePtr` is its session's, and another session's is a runtime error. `funk_free` returns all of a session's memory: every compiler allocation carries a 16-byte header on its session's list, about 2 MB per live session, and 30 sessions made and freed in turn left resident memory flat, where each had kept 9 MB.

  ```c
  funk *f = funk_new(&(funk_config){.heap = 512 << 20});
  int status = funk_run(f, body, arg); /* body(f, arg) on f's thread */
  funk_free(f);
  ```

### Fixed

- funk runs under AddressSanitizer (`-DFUNK_SANITIZE=address`) with no reports across the suite and the plugin's test. Three things in the runtime had made ASan report or fail: the collector's copies and scans of the stack read beyond live frames, which ASan forbids, so they are now excluded from its checks and copy word by word, since ASan checks `memcpy` wherever it is called; on a stack as large as funk's, ASan does not clear the frames a `longjmp` abandons, and their stale redzones reported overflows after every runtime error, so funk clears them before each `longjmp`; and each session's thread replaced ASan's alternate signal stack with a `malloc`ed one that ASan then failed to unmap, so funk now uses a thread's own when it has one. ASan's VLA checks are off: it aligns each VLA to 32 bytes, and in 4% of runs `FUNK_GCSTRESS`'s check found a stale pointer in the padding of `eval`'s frame, a slot the program never reads. CI runs the suite under ASan on Linux and macOS. Three tests are disabled there, as ASan changes what they check: its warning in a compared output, and resident memory in `embed/memory`; `embed/sessions` runs without ASan's quarantine, and hosts that load the plugin are built with the sanitizer, which macOS needs in the process from the start.

## [0.7.0] - 2026-10-30

### Added

- The `funk-midi` CLAP plugin moved here from `midi-langs`, with a copy of `midi-langs`' plugin framework (`projects/clap`, `projects/funk-clap`). `-DFUNK_CLAP=ON`, or `make clap`, builds it from the vendored `thirdparty/{clap,pugl,imgui,libremidi}`; `tests/hosts/clap` tests it. The plugin will gain audio ports (gh:3), which `midi-langs`' note effects do not need; the copy keeps that change out of their framework ([docs/dev/plugin.md](docs/dev/plugin.md#decision)).

- `funk_provide(name, addr)` in `libfunk.h`: foreign imports of the C name `name` use the host's function or variable at `addr`. They found a host's functions as symbols of the process, which a plugin does not export. `funk-midi` provides `funkgraph`'s functions this way, and no longer exports its symbols.

- `make install` installs `libfunk.a`, `libfunk.h` and a CMake package: `find_package(funk)` gives `funk::libfunk`. The version must match exactly, since `libfunk.h` has no stability promise between releases.

- `lib/pattern.fk`, a pattern library imported with `-M lib` ([docs/dev/patterns.md](docs/dev/patterns.md)). A pattern is a query from a span of time to the events overlapping it, as in Tidal. Time is in integer ticks, 720720 per cycle, rather than rationals: arithmetic stays in `Int`, and subdivisions up to 16 are exact. `sampleEvents`, `pushOnsets` and `writeEvents` turn a block of samples into events for an export's ring or a host's buffer.

- `funk_call` in `libfunk.h`: calls a global from `Int`s to `Int` without compiling. A host that called `funk_eval` once per audio window grew by about 16 KB per call, since every REPL input stays compiled; `funk_call` allocates nothing in the compiler.

- `funk-midi` (`projects/funk-midi/main.c`): a REPL that plays `pat`, a `Pattern Int`, to a MIDI port, and replaces it live when it is redefined ([docs/dev/patterns.md](docs/dev/patterns.md#funk-midi)). MIDI output needs `-DFUNK_LIBREMIDI=ON`, which builds the vendored `thirdparty/libremidi`; without it, it prints. `lib/midi.fk` gives velocity and channel. On macOS its sender is a time-constraint thread: as a default thread, messages went out up to 8 ms late.

- `funk_output` in `libfunk.h`: program output and messages (compile and runtime errors, warnings, `trace`) go to a host's function, one call per message, instead of stdout and stderr. A host with its own console, such as a terminal editor or a plugin, needs it.

- `funk_interrupt` in `libfunk.h`: from any thread, stops the running evaluation with the runtime error "interrupted". The interpreter checks a flag at each call, so a loop stops; a long primitive finishes first. A host's Stop needs it: an endless loop typed in a plugin's editor otherwise holds the evaluator for good.

- `funk_memory` and `funk_on_fatal` in `libfunk.h`. `funk_memory` sets the heap's limit, the first collection's threshold and the stack's size; at the limit, allocation runs a full collection, then raises the runtime error "out of memory", which a host recovers from. Before, the heap's only limit was its 64 GiB address-space reservation, so a runaway program took the host's memory, and running out ended the process. `funk_on_fatal` gives a host the message of an error funk cannot recover from, such as `malloc` failing, before the process exits.

- The heap returns memory to the system. A chunk sweeping finds all dead becomes its size class's bump region, as before, if that has none; otherwise it now serves any size class, where before its objects went onto its own free list. Empty chunks beyond the collection threshold are replaced by fresh pages, and after "out of memory" a full collection runs at once. A host that met a 512 MiB limit kept 589 MiB resident; it now keeps 217 MiB. Sending every all-dead chunk through the empty list cost `lists` and `wordfreq` 1-2% (interleaved A/B, 21 runs); with the bump-region route kept, the core suite's geometric mean against HEAD is -0.0%.

- `funk emit-c --kernels FILE`: FILE's foreign exports and what they call, as C without the runtime, for a host to load as unit generators ([docs/dev/audio.md](docs/dev/audio.md), D). An export's code read field offsets and other top-level values from the runtime's global table, filled at startup, so it could not run outside a funk program. In a kernel they are C expressions and inlined initializers; a top-level value an export reads must therefore pass the export check too. Four kernels compile in 70 ms at `-O2`, against 425 ms with the runtime pasted. The first line names the libraries to link, such as `/* link: -lm */`: the `foreign library` names, then libm, which the interpreter's glue also links.

- `projects/funkgraph/graph.h`, a runner for graphs of kernels ([docs/dev/audio.md](docs/dev/audio.md), D). A kernel takes its state, its inputs, parameters included, its output, a frame count and a context holding the sample rate. Parameters are inputs rather than state so that they can be modulated, and so that changing one does not restart an oscillator. The host's thread builds a graph from kernels, their initial states and earlier nodes' outputs, and plays it; the audio thread renders the current one and allocates nothing. A replaced graph is freed by the host's thread once the audio thread has finished a block after the swap, since the block running at the swap may still read it.

- `lib/graph.fk`: builds and plays such graphs from funk, interpreted or compiled. A `Sig` is a tree of nodes, each a kernel's name, found with `dlsym` in the player's libraries, its initial state as `Float`s and its inputs; `play` writes it into a new C graph and swaps it in. A node given a name (`named`, or `funk_graph_name` in C) is built once however often the tree uses it, and keeps its state across a swap while its kernel and initial state are unchanged, so editing one part of a graph does not restart the oscillators elsewhere. The audio thread copies those states at the new graph's first block, since the host's thread cannot read a state being written; a replaced graph is therefore freed one block later. `projects/funkgraph` is now the CMake library `funkgraph`, which the import glue links.

- `lib/dsp.fk`: oscillators (sine, saw, square, pulse), one-pole and state-variable filters, an ADSR envelope and arithmetic, as signals for `lib/graph.fk`; `Sig` is a `Num`, so `lowpass 2000 (dc 0.7) (saw 220) * adsr ...` is a graph. The kernels are `lib/kernels.fk`, compiled with `funk emit-c --kernels`; a separate module, since an interpreted program cannot import a file with exports.

- `funk-midi --audio`: input that defines `sig`, a signal of `lib/dsp.fk`, plays it from the next audio block, beside `pat`'s MIDI. The host owns the player; funk only builds graphs, so its collector and compiler never run on the audio thread. The device is miniaudio's, vendored in `thirdparty/miniaudio` (0.11.25) and on by default; with `-DFUNK_MINIAUDIO=OFF`, `--audio` runs only on the virtual clock (`--no-sleep`), printing the output's peak. CMake compiles `lib/kernels.fk` into `funkdsp.so` with the new `tools/kernels.sh`.

- The CLAP plugin has a stereo output: `sig` plays there, and every note it sends also plays its voices (`voices f`, 8 of them), at the note's frame. Notes are taken where the plugin emits them, so clips, the host's transport, offline render and Stop reach the voices as they reach the MIDI output. Without a C compiler for graph.fk's imports, MIDI still plays and the output is silent ([docs/dev/plugin.md](docs/dev/plugin.md#the-plugins-audio)). The plugin keeps its modules in `$XDG_CACHE_HOME/funk/plugin/<hash>`: in a new temporary directory per load, which the glue's compiler arguments name, every load compiled the glue again.

- `d.polyOn c n f` in `lib/dsp.fk`: `d.poly` for MIDI channel `c`, whose notes then play it instead of the shared voices; the plugin and `funk-midi --audio` define `voicesOn c f`. A note-off now also matches its channel: an off on channel 2 ended the oldest held note of that pitch, which could be channel 1's. Routing is by whether the playing graph has channel `c`'s first control, so `voices f` needs no list of channels it leaves out.

- The CLAP plugin takes notes from the DAW, as CLAP note events or MIDI, and plays them on its voices with its own notes. A block's notes are sorted by frame before they reach the voices, which take events in time order: a clip's note at frame 0 handled after a host's at frame 300 started at 300.

- `make clap-dev` plays an instrument plugin's audio output on the default device (miniaudio), whose callback then runs `process()`; `--meter` prints its peak every second. Before, the dev host passed no audio buffers, so the funk plugin's output was silent there. The editor's default code defines a `sig` of voices: with only `pat`, notes went out as MIDI and the plugin rendered nothing.

- `funk emit-c --glue FILE` and `funk_glue` in `libfunk.h`: the glue of FILE's foreign imports, as C a host links in, so the interpreter runs no C compiler for them. Each import is found by a key over its conversions, C name and header, not by its slot in the runtime's globals, which depends on what was loaded before; an import whose key is missing is compiled with `$CC` as before. The CLAP plugin links graph.fk's glue: before, it needed a compiler in the DAW's process, which may have none or forbid running one ([docs/dev/plugin.md](docs/dev/plugin.md#linked-glue)).

- `funk_voices` in `projects/funkgraph/graph.h`: notes on `d.poly`'s voices, moved from `funk-midi` so that the plugin shares it. A note's off is given with it or by a later note-off, as MIDI sends it.

- `funk_player_send_audio` in `projects/funkgraph/graph.h`: events made on the audio thread, such as a plugin's notes, in a queue of their own. `funk_player_send`'s queue allows one producer, and `g.send` already uses it from the host's thread. Rendering merges the two queues by frame, since an event sets its control to the end of the block.

- Controls in `projects/funkgraph` and `lib/graph.fk`: a node without a kernel whose value events change at a given frame (`funk_player_send`, `g.send`), so a note's gate or frequency changes at its sample rather than at a block's start. Events go through a queue of C structs in the player, a 64-bit frame, a name and a `float` each, rather than funk's `Ring Int`, whose values have 16 bits: the runner, not a funk export, reads them.

- Voices: `d.poly n f` sums `n` voices of `f gate freq`, each reading its own controls, and `funk-midi --audio` sends `pat`'s notes to them (`voices f`, sized by `--voices N`, default 8). A note takes the free voice released longest ago, else steals the oldest; a reused voice's gate drops for one frame so its envelope restarts. A fixed pool in one graph, rather than nodes added per note, so a note never swaps the graph. Notes are timed in the player's frames, which the host tracks against its clock in real time.

- `bench/dsp.c` (`build/dspbench`): the cost of each kernel, of the graph per node, and of a voice in funk kernels, the same kernels in C, and one fused C loop ([docs/dev/261029-dsp-measurements.md](docs/dev/261029-dsp-measurements.md)). On an M1 at 64 frames, a voice costs 792 ns in funk, 816 in C kernels and 292 fused.

- Channels in `lib/graph.fk`: `g.Channels [Sig]`, or `d.stereo l r`, is a signal of several channels, and a node with such an input becomes one node per channel, as in SuperCollider. A mono input is added once and shared by the channels' nodes, rather than copied per channel, so `d.pan pos voice` does not build the voice twice. `funk_graph_output` and `funk_player_render_channels` in `projects/funkgraph`; a mono graph plays on every channel. `funk-midi --audio` plays stereo.

### Changed

- `libfunk` exports only `funk_*`; it is built with `-fvisibility=hidden` and position-independent. It exported its compiler's and runtime's internals too (`repr`, `lex`, `globals`, `fk_cons`), which a plugin host or a second plugin in the same process could bind to.

- `peekByteOff` and `pokeByteOff`, and so `readArray`, `readField` and their kin, compute the address as `char *` arithmetic rather than through `plusPtr`'s integers. The cast lost the pointer's provenance, and clang did not vectorize: a kernel multiplying two buffers ran 3.9x hand-written C, a state-variable filter 1.34x; both now equal it. `fieldPtr` does the same unless its pointer is null, where it still gives the offset: a kernel whose state is a struct of two structs went from 2.6x C to 1.0x. `plusPtr` keeps the integers, since `plusPtr nullPtr 16` must stay defined. The Benchmarks Game is unchanged (+0.1%, interleaved A/B).

### Fixed

- Foreign imports inside a plugin that embeds `libfunk`. The interpreter's glue found the runtime's functions as symbols of the process, so in a module that exports only its entry point, or one loaded `RTLD_LOCAL`, it failed to load; with two such modules loaded `RTLD_GLOBAL`, the second's glue called the first's runtime. Glue now reaches the runtime through pointers set when it is loaded, and each loading image compiles its own glue.

- funk's `SIGSEGV` and `SIGBUS` handler passes faults that are not its own to the handler installed before it. It reset them to the default action, so in an embedding host a fault the host would have handled, such as on its own guard page, ended the process. When `funk_main` returns, the handlers it replaced are installed again, so a plugin that is then unloaded leaves none pointing into it.

## [0.6.0] - 2026-10-27

### Added

- GitHub Actions CI (`.github/workflows/ci.yml`): `make test` on Linux and macOS, plain and with funk built under UBSan.

- Modules, phase 1 ([docs/dev/modules.md](docs/dev/modules.md)). `import a.b` compiles `a/b.fk`, found beside the importing file or under `-M DIR`, and makes its names available qualified by the last segment, or by the name after `as`: `b.f`, `b.T`, `b.C`, `x b.<+> y`. Qualified-only, since a name added to a module then cannot clash in the files that import it. Every module is compiled once per program, and whole-program optimization still crosses module boundaries: a module's list pipeline fuses as before.

  ```haskell
  import shapes.geometry
  main = print (geometry.area (geometry.Circle 2.0))
  ```

- Import and export lists, modules phase 2. `import m (f, T(..), C(..), (<+>))` also brings those names in unqualified. `module m (f, T, C(..)) where` exports only those. A type exported without `(..)` is abstract: its constructors and fields stay inside its module, and its namespace functions go with it. A type not exported keeps its fields and namespace functions inside even when its values leave the module.

- Modules phase 3: `import` at the REPL prompt, and `:l` puts a file's imports in the REPL's scope. Haskell's `module Main (main) where` and base imports are accepted: `import Data.List (sortBy)` and `import qualified Data.Char as C` check their names against the prelude, which stands in for those modules; `C.ord` is `ord`.

- `bench.py ab OLD [NEW]`: compares two compilers by running their binaries alternately, which cancels the drift of about 5% between sessions (`bench/README.md`).

### Changed

- `pipeline` runs at n = 100,000,000, 0.3 s, up from 300,000, 3-6 ms, which was too short to time; old rows are not compared. `report` judges only funk's rows, since nothing in funk changes C's.

- regex-redux uses PCRE2, not PCRE, which Homebrew has deprecated and recent Linux releases drop: funk through foreign imports, and the Game's PCRE2 C program (`gcc-5`), built without OpenMP. Homebrew's PCRE had no JIT on arm64 (`gcc-2` took 1.50 s on fasta's output for 1,000,000, `gcc-5` 0.56 s); funk takes 0.58 s.

### Fixed

- An effectful function could not call a parameter declared pure: `each :: (Int -> ()) ->> ()` with `each f = f 1` was rejected, since the call merged the signature's shared `->` effect into the body's `Eff`. A body that is already `Eff` takes a call of any effect now (`docs/dev/typed-effects.md`).

- The effect error for a signature's `->` names the arrow, by the arguments before it: "its signature's arrow after 2 arguments is `->`; it needs `->>`". A returned function that acts at a `->` reports "where its signature has `->`".

- `show` parenthesizes `-0.0` as an argument, as GHC does: `Just (-0.0)`, not `Just -0.0`.

Found by the first Linux run:

- The compiler passed NULL to `memcpy` and `memmove` with a size of 0, from empty arrays, in 24 places; undefined in C, and reported by UBSan against glibc's `nonnull` declarations, which macOS's headers lack. They go through `copy_n` and `move_n` now.

- Generated C failed GCC's `-Wall -Werror`: a worker that only loops had no `return` (`-Wreturn-type`; each ends in `__builtin_unreachable()`, with unchanged machine code), and `fk_ptr` took a `const void *`, from which GCC concluded that `mallocBytes`' fresh memory was read uninitialized.

- `tests/run/overflow.fk` recursed through `1 + 2 * _`, which GCC turns into a loop with an accumulator, so it ran 2 billion steps instead of overflowing; it uses `mod` now.

- The reuse and fusion tests sorted their lists by the locale; they sort by bytes (`LC_ALL=C`).

## [0.5.0] - 2026-10-27

### Added

- A second benchmark suite, `bench/game/`: the Benchmarks Game's 10 tasks, against its single-threaded C programs (`bench.py --suite game`). Each matches the Game's output file in both backends, in `make test`. regex-redux and pidigits use PCRE and GMP through foreign imports, as their C does. Geometric mean 1.16x C: binary-trees 0.4x, spectral-norm 0.8x, mandelbrot, regex-redux and pidigits 1.0x, fannkuch-redux 1.1x, n-body 1.2x, reverse-complement 2.1x. See [bench/README.md](bench/README.md#benchmarks-game-suite).

- Bitwise operations on `Int`, with `Data.Bits`' names and fixities: `.&.`, `.|.`, `xor`, `complement`, `shiftL`, `shiftR`. A shift by 64 or more gives 0 (or -1 for `shiftR` of a negative number), as GHC's; a negative shift is an error, so a shift by a variable is not treated as total.

- `isEOF ()`, for reading standard input line by line to its end; `getContents ()`, all of it at once, strictly; `indexString s i`, one character of a `String` without unpacking it; `writeArrayString arr i s`, a string's bytes into an `Array CChar` in one copy; `putArray arr i n` and `getArray arr i n`, a range of an array to standard output or from standard input; `copyArray src i dst j n`, as `memmove`.

- `IntTable` in the prelude: a mutable hash table from `Int` to `Int` in C memory, written in funk over `Array` (open addressing; doubles at three quarters full).

### Changed

- Reuse of matched cells extends to every data type, to the elements of a list nothing else references, to the components of a tuple result, and to any allocation of the same size, not only list cells at TRMC sites. A variable used at most once on each path owns its value, so a function may also return or store what it was passed. n-body allocates nothing per step: 2.8x to 1.2x C, 134 to 2 MB; `records` -22.5%. `bst`'s inserts now update in place, and its lookups are 27% slower on the resulting layout (+8.6% in all). See [docs/dev/element-reuse.md](docs/dev/element-reuse.md).

- A function whose every result is a tuple it builds returns it as a C struct in compiled code, so a `case` on its call allocates nothing; other uses box the tuple. Interleaved runs: n-body -30% (4.0x to 2.8x C), fasta -11%; the core suite -0.3%.

- `newArray` writes its first element and fills the rest by doubling copies (`copyArray`), and skips the fill when that element's bytes are zero, since the memory starts zeroed: an array's pages are then touched only when used. reverse-complement, reading in chunks into such a buffer, went from 0.49 to 0.25 s and 373 to 288 MB.

- Generated workers are `static inline`, except TRMC loops: the keyword raises clang's inlining threshold at `-O2`, so the prelude's small wrappers, such as `readArray`'s chain to `peek`, inline into loops. Interleaved runs: fannkuch-redux -53%, reverse-complement -31%, k-nucleotide -30%, records -9%; the core suite -0.8%, the game suite -14%. Inlined TRMC loops were up to 8% slower (sort's `randoms`), so they stay out of line.

- `readArray` and `writeArray` build their index error in a separate function, so the C compiler inlines them: spectral-norm -44.6% (now 0.8x C), fannkuch-redux -28%, k-nucleotide -26.5%. `newArray` fills with a loop instead of `mapM_` over `enumFromTo`, which built the list of indices first (300 MB for 12.5M elements). `getLine` reads a line with `getline(3)`, not a character at a time.

- A local binding whose right side calls a global function is evaluated where it is bound, not made a thunk, when that function's body is itself safe to evaluate early: `let m = max a b` at `Int`. A function that recurses, directly or not, or calls a parameter, still makes a thunk. `fannkuchredux` no longer allocates (135 to 2 MB resident); `fasta` -13.7%.

### Fixed

- A `data` constructor may hold a `foreign struct`'s type (`data St = St (Ptr Mpz)`); it was an unknown type, since data types were registered before structs. A `data` and a struct of the same name are still an error.

## [0.4.1] - 2026-10-27

### Changed

- funk is a static library, `libfunk.a`, with an internal api in `src/libfunk.h`, and a command line, `cli/main.c`, that uses only that api. The api returns errors and never exits. It is internal, with no stability promise, since the compiler's global tables allow one session per process.

- The test suite is one ctest test per check, so `make test` runs them in parallel (63 s to 21 s on 8 cores), and `ARGS="-R NAME"` or `ARGS="-L LABEL"` runs a subset. Each test has its own foreign-import cache and a 300 s timeout. `tests/check.sh` replaces `tests/run.sh`.

- Generated C defines nullary constructors (`[]`, `LT`, `Nothing`, a user's `Red`) as constant objects, not objects made at startup and read through the runtime's constructor table; clang sees their ids, and `sort`'s code does 5 fewer table loads. Interleaved runs: `lists` -9.6%, `sort` -5.4%, `wordfreq` -4.9%.

- `Int` boxing in generated C hints that a value fits in 63 bits (`__builtin_expect`): geometric mean -1.0% over the benchmarks, `sort` -5.3%. Of the 2.7% that 64-bit `Int` still cost, 0.7% remains; see [docs/dev/typed-backend.md](docs/dev/typed-backend.md#integers).

- Sweeping a chunk with nothing marked makes it a bump region instead of threading each dead object onto a free list. In `lists`, which drops every list each round, lazy sweeping fell from 209 to 7 ms of 950. Interleaved runs: `lists` -8.9%, `trees` -8.4%, `records` -6.3%, geometric mean -1.6%.

- A `case` in generated C tests `[]`, `:`, `Nothing`, `Just` and other constructors of types with at most two nullary ones by comparing addresses, not by loading the constructor id: nullary constructors are unique constants. A list loop's two tests become one compare, and clang drops the unreachable match failure. Interleaved runs: `sort` -12.3%, `lists` -11.1%, `wordfreq` -2.8%, geometric mean -2.0%.

- `show` at `Int` is its own primitive, `primShowInt` in C as `showi`, and typed code calls it on an `int64_t`: digits into a stack buffer, then one string, where it went through `snprintf`, a heap buffer filled byte by byte, and a copy. It allocates, so exports still reject it. Interleaved runs: `strings` -43%, `printing` -34%, `wordfreq` -6.8%.

- The C versions of `printing`, `strings` and `wordfreq` format numbers with the same digit loop as funk's `show` at `Int`, not `snprintf`: with funk's new `show`, they made funk read 0.6-0.9x C. The geometric mean is 1.19x C; their older C rows are not comparable.

### Fixed

- Generated C compiles without warnings under `-Wall -Wextra` (1,312 before, over the run tests, examples and benchmarks). `case` emits only the labels it jumps to; the program section ignores `-Wunused-variable` and `-Wunused-but-set-variable`, for temporaries a pattern does not read, which clang removes; warnings in foreign-import glue still show. The machine code is unchanged. A `wall` test per run test and example keeps it so.

- Incremental marking steps once per 256 KiB of allocation, not 1 MiB. Nothing is freed until a cycle ends, so a long marking window took fresh chunks: `bst` ran 6% slower than before 0.4.0's collector, with a 37% larger resident set. It is now 1.6% slower; the 0.4 ms pause bound is unchanged. Details in [docs/dev/realtime.md](docs/dev/realtime.md).

- `:l` with an unreadable file in the REPL reports it and continues; it ended the REPL.

## [0.4.0] - 2026-10-27

### Added

- `foreign export ccall "cname" name :: type` makes `cname` a C function that runs `name`, callable from any thread, for callers with deadlines such as audio callbacks. The compiler checks that the function's compiled code cannot allocate, and names what would: lists, strings, closures, lazy bindings, unknown calls. `Ptr`, `FunPtr` and `StablePtr` are now C addresses in typed code, and `castPtr`, `plusPtr`, `peek` and `poke` at scalar types compile to C, so pointer code no longer allocates. A literal at a known `Num` instance, such as `0 :: CInt`, calls its `fromInt` without a dictionary. A runtime error on a foreign thread aborts the program. See [docs/dev/realtime.md](docs/dev/realtime.md), R4.

- `droppedEvents ()`: queued callbacks that found the event ring full.

- The export check also rejects recursion other than a self call in tail position, a loop, so an export's stack use is bounded by its call graph.

- The export check also rejects code that can fail, since a runtime error on a foreign thread ends the program: `div`, `mod`, `quot` and `rem` by anything but a non-zero literal, `chr`, `floor` and the other partial primitives, and a `case` that does not cover every value. It accepts enumerations such as `Ordering`, whose values are static.

- `Array a`: a fixed-length array of `Storable` values in C memory, freed with `freeArray`. `arrayLength` acts (`->>`), since it reads memory `freeArray` frees. `readArray` and `writeArray` check the index; `readArrayWrap` and `writeArrayWrap` take it modulo the length and cannot fail, so a foreign export may use them. In typed code an array is a C address. C memory, over the funk heap, because a collected array could be freed while a foreign thread reads it.

- `Ring a`: a single-producer, single-consumer ring of `Storable` values in C memory, between any two threads, such as funk's thread and an audio callback. `ringPush` returns `False` when full; the consumer uses `ringCount`, `ringFront` and `ringDrop`, three calls where a `Maybe` would allocate.

- `sizeOf` in `Storable`, as in Haskell; `Storable` instances for `Array a` and `Ring a`.

- `truncateSat :: Float -> Int`: truncation that saturates at `Int`'s range and maps NaN to 0, so it cannot fail.

- `foreign struct ["header.h ctype"] T { field ["c.member"] :: type, ... }` declares a C struct: the header's, with its field types checked, or one laid out as C would. A field may be another foreign struct, held by value and reached with `fieldPtr`. `T.field :: Field T a` is a field's offset, from `offsetof`, and `T.size` the size; `readField`, `writeField` and `fieldPtr` use them, also in exports. C computes the layout, over funk, so it matches the platform's alignment rules.

- `atomicAdd` and `atomicCompareSwap` in `Atomic`, sequentially consistent; floats through a compare-and-swap loop.

- `Atomic`, with `atomicPeek` and `atomicPoke`: an acquire load and a release store, for memory shared between funk's thread and a foreign export. Instances for `Int`, `Float` and the C types.

- [examples/ffi/portaudio.fk](examples/ffi/portaudio.fk): a wavetable synth whose PortAudio callback is a foreign export, fed frequency changes through a `Ring`; 0 underflows in three runs, callbacks of at most 5 us against a 1.33 ms buffer, with four collection cycles during each run.

- `FUNK_GCSTATS=1` prints each collection cycle's pauses, and a summary at exit, in both backends. `FUNK_GCSTRESS=1` runs short collection cycles constantly and stops the program if a reachable object is left unmarked, for testing the collector.

- Foreign imports, phase 1 of [docs/dev/ffi.md](docs/dev/ffi.md): `foreign import ccall "math.h cbrt" cbrt :: Float -> Float` calls a C function, and `foreign library "name"` links `-lname`. Arguments and results are `Int`, `Float`, `Char`, `Bool`, `()` and the C types below; `->>` marks an import that acts. An import of a non-function type reads a C value once (`"math.h M_PI"`). Compiled code calls the C function directly, as fast as a primitive. The interpreter compiles the same glue with `$CC` into a shared library, cached by its hash in `$XDG_CACHE_HOME/funk/ffi`, and loads it: about 90 ms the first time, nothing after. Compiled glue, over libffi in the interpreter, lets imports call macros and `static inline` functions, and keeps both backends' conversions identical. `funk run` and `repl` take `-I`, `-L` and `-l` for the glue, and `build` takes `-I`.

- The declared types of an import are checked against the C declaration by the C compiler: a conversion that can change a value or drop a `const` is an error, so `abs :: Int -> Int` is rejected, since `abs` takes an `int`. Widening is allowed, and a `Ptr` passes as any object pointer. An exact comparison of the function type was tried first; it cannot pass `Ptr Db` where C has `sqlite3 *`.

- Foreign imports, phase 3: callbacks. `foreign import ccall "callback" f :: FunPtr (Ptr () -> a ->> r)` is the address of a C function that runs a funk closure, passed in its context argument as a `StablePtr`, on funk's thread; `"queue"` makes one that any thread may call, whose calls `waitEvent` and `pollEvent` run later. `CStringLen` in a callback's type is a C pointer and length, received as a copied `String`. An error in a callback is raised when the foreign call returns, never across C frames. A value import may be any C expression, such as `sizeof(T)` or `offsetof(T, f)`, for `peekByteOff` and `pokeByteOff`. Also: `allocaBytes`, `newCString`, `CBool`, `newStablePtr`, `deRefStablePtr`, `freeStablePtr` and the `FunPtr` and `StablePtr` casts. libremidi works ([examples/ffi/libremidi.fk](examples/ffi/libremidi.fk)). A trampoline per callback type, with the closure in the context, needs no code generated at run time, unlike Haskell's `"wrapper"`; C APIs without a context pointer cannot call funk.

- Foreign imports, phase 2: `String` arguments and results (`const char *`), `Ptr a`, opaque types (`data Db` without constructors), and Haskell's `Storable` (`peek`, `poke`), `alloca`, `mallocBytes`, `free`, `nullPtr`, `castPtr`, `plusPtr` and `peekCString`, with `CChar`. A NUL inside a `String` passed to C, or a NULL `String` result, is a run-time error rather than a shortened string. sqlite3 works in both backends.

- `CInt`, `CUInt`, `CLong`, `CULong`, `CLLong`, `CSize` and `CFloat`, C's scalar types, with `Eq`, `Ord`, `Show` and `Num` instances whose arithmetic wraps to the C width. `CULong` and `CSize` compare and show unsigned. A new class `Integral`, with `toInt`, and `fromIntegral :: (Integral a, Num b) => a -> b` convert between integer types; `toCFloat` and `fromCFloat` between `Float` and `CFloat`. These names are now reserved. Real types, over names that mean a range-checked `Int` in foreign signatures only, keep Haskell bindings close to their source and give C's wrapping arithmetic.

### Changed

- The glue cache rebuilds when a header the glue read is newer (from the compiler's `-MD` list), or a library named with `-l` in an `-L` directory has changed.

- A NULL `String` passed to a queued callback is an error raised after the event's slot is released, so the ring moves on.

- `fk_cb_check` in code an export runs on another thread leaves funk's thread's pending callback failure alone.

- A struct holding a header's struct by value includes that header; a failed REPL chunk's foreign structs are forgotten.

- A collection cycle's start pause no longer scans the stack: it makes the stack's older pages read-only, a write copies a page before it proceeds, and marking steps scan the pages. At a million frames the start pause fell from 28.9 ms to 0.2 to 0.27 ms and the longest pause is 0.47 ms. Protection over a `memcpy` of the stack, which a first version used: 0.2 ms against 9 to 12 ms for 96 MiB. The kernel cannot write into a read-only page (`EFAULT`), so a foreign call first makes the pages below its frame writable, and while a direct callback runs the stack is copied instead. Protection stops below the funk thread's first frame, above which glibc keeps thread data the kernel writes.

- `FUNK_GCSTRESS`'s check maps fresh pages over the dead stack at each cycle's start: unset slots in new frames held words of dead frames, which it reported as unmarked objects. `madvise(MADV_DONTNEED)` zeroes them on Linux but not on macOS. Its steps scan the stack's snapshot a chunk at a time, so a stressed run repeats.

- `plusPtr` adds as integers: C's pointer arithmetic on a null pointer, as in `plusPtr nullPtr 16`, is undefined.

- The collector sweeps lazily: allocation sweeps a chunk when its free list runs out, so a pause is roots and marking only. The longest pause of a program with 100,000 live cells fell from 17.9 ms to 1.0 ms, and with 1,000,000 from 35.0 ms to 9.8 ms; a sweep step costs at most 0.15 ms. Allocation-heavy benchmarks ran 2-8% faster. A chunk still unswept at the next collection has its marks cleared rather than being swept, which would put the sweep back in the pause. See [docs/dev/realtime.md](docs/dev/realtime.md).

- Queued callbacks go through a fixed ring of 4096 events: a trampoline claims a slot with a compare-and-swap and writes its arguments into it, where it called `malloc` and took a mutex. A full ring drops the event and counts it; bytes over 104 per event still use `malloc`. A callback on a real-time thread then waits on nothing funk holds.

- The collector marks incrementally: after a pause that scans the stack and the roots, it marks in steps of at most 0.4 ms, one per MiB allocated, and sweeping finishes in steps of the same size before the next cycle marks. The longest pause no longer grows with the heap: 0.40 ms with 1,000,000 or 4,000,000 live cells, where lazy sweeping alone left 9.8 and 37.5 ms. The start pause still grows with the stack, 29 us per 1,000 frames, and the heap is up to 70% larger. Three writes that overwrite a heap pointer, a thunk's update and a reused cell's head and tail, shade the old value while a cycle marks; compiled throughput changed by -0.2% to +7.9%, within what code placement alone moves. Snapshot at the beginning, over Dijkstra's insertion barrier, needs barriers only at those writes, since funk's other stores initialize new objects. See [docs/dev/realtime.md](docs/dev/realtime.md).

## [0.3.0] - 2026-10-26

### Changed

- Prelude functions and class methods cannot be redefined at top level: a definition of `lookup` or a method named `length` is an error, where it used to replace the prelude's. A name in a program then always means the prelude's, since without modules a shadowed prelude function could not be reached. Use another name or a type namespace (`Shape.lookup`); locals may still shadow prelude names. The prelude's types and constructors (`Maybe`, `Just`, `Ordering`) are reserved likewise.

- Effects are in types. A function arrow is `a -> b` if the function does not act by itself and `a ->> b` if it may; effects are inferred, and checked against signatures. A signature's `->` arrows share one effect, so `map :: (a -> b) -> [a] -> [b]` acts exactly when its argument does. Breaking: a signature that says `->` over a body that prints is an error; a top-level value that acts is an error, except `main` (make it a function); a `where` or `let ... in` binding that can act is an error, since its effect would run at a later use or under the wrong guard: bind it with `<-` or a `let` statement in `do`, where it runs on its line, or make it a function; a data field or class method written with `->` holds only pure functions. Chosen over Haskell's `IO` type because calls stay direct, with no runtime cost, and unannotated code needs no change. Design, prototype and alternatives: [docs/dev/typed-effects.md](docs/dev/typed-effects.md).

## [0.2.0] - 2026-10-25

### Changed

- Fresh heap chunks are allocated with a bump pointer instead of a free list built when the chunk is created, which wrote every object once before its use. The geometric mean over C improved 3%; `lists` 9%.

- Copies of higher-order functions made for a known function argument also take the call site's types, so `flip@{0=primMulInt}` works on C integers instead of boxed values. Copies are keyed by those types too, so `map id` at `[Int]` and at `[Char]` get separate copies. The geometric mean over C improved 5.7%.

- `Int` is 64 bits in both backends and wraps on overflow. Before, the interpreter wrapped at 63 bits while compiled code computed in 64 bits and truncated to 63 when an `Int` was stored in a list or passed to polymorphic code, so results after an overflow differed between backends and within compiled code. Values beyond 63 bits are boxed; integer literals and `readInt` accept the full 64-bit range. Chosen over 63-bit wrapping in compiled code, which measured 29% slower against 5.4% for boxing. The geometric mean over C went from 1.37x to 1.48x.

- Constructor fields declared `Float` are stored unboxed in compiled code: `data Vec = Vec Float Float` takes 3 words instead of 3 words plus two 2-word boxes, and reading a field does not follow a pointer. A program building 2M such values runs 2.8x faster. The interpreter keeps boxed fields.

- Fixed: a negative-zero literal, `(-0.0)`, compiled to `0.0`. The code generator printed Float literals with `%g`, which renders `-0.0` as the integer `-0`; it now emits hex float literals, which are exact.

- Closures and thunks store captured `Int`, `Float`, `Char` and `Bool` values raw instead of boxed, so capturing a `Float` no longer allocates. The benchmarks do not change; a program building 2M closures over `Float`s runs 8% faster.

- Compiled functions take and return C scalars: each is a worker with typed parameters, called directly, and a uniform wrapper for closures and unknown calls. clang can now inline small functions such as `even` and `compare`. The geometric mean over C went from 1.52x to 1.37x; `fib` is at C speed. One effect: clang turns some non-tail recursions, such as `1 + f (n - 1)`, into loops, so compiled code may run them in constant stack where the interpreter overflows.

- Compiled and interpreted code fuse chains of prelude list functions into one loop, without the intermediate lists. Chains are built from `enumFromTo`, `replicate` or any list, through `map`, `filter`, `take`, `takeWhile` and `concatMap`, into `foldl`, `sum`, `length`, `elem`, `any` or `all`, or back into a list. A chain is fused only when the result, the error raised and termination are unchanged: no stage performs an effect, at most one stage can fail, and no stage before `take` or another short circuit can fail. Exact semantics were chosen over Haskell-style imprecise errors, which would fuse more chains but make error messages depend on the optimizer. `FK_NOFUSE=1` turns fusion off. The benchmark `lists` is split: `pipeline` is the old program, compared with a fused C loop, and `lists` names its lists so they stay. The geometric mean over C went from 1.68x to 1.52x; `queens` from 2.9x to 1.4x. Design: [docs/dev/fusion.md](docs/dev/fusion.md).

- Compiled code writes the output of `map`, `filter`, `take`, `zip` and similar functions into the cells of their input list, when that list is fresh: the direct result of a call that builds new cells, such as `filter even (enumFromTo 1 n)`. A static check proves that nothing else references the input; a variable never counts as fresh. Chosen over Perceus reference counting, which a prototype showed slower: freeing cells as they die scatters later allocations. Separately, an alternative that rebuilds the cell it matched (`merge`'s `x : xs`) returns that cell. The geometric mean over C went from 1.79x to 1.68x; `sort` is 30% faster. Design: [docs/dev/reuse.md](docs/dev/reuse.md).

- `String` is a packed builtin type, no longer `[Char]`. `++` and `length` are class methods with instances for lists and `String`; `words`, `lines`, `unwords`, `unlines` and the new `concatStrings` are primitives. List functions need `unpack :: String -> [Char]`, and `pack` converts back. A string literal pattern compares by equality. Chosen over keeping `[Char]` for speed: one 3-word cell per character becomes one byte. `strings` runs at 1.6x a packed C version; it was 3.3x a `[Char]` C version.

- Generated code is faster: on the benchmark suite, its geometric mean over C went from 10.3x to 1.35x (`docs/dev/performance-first.md`). Local bindings that cannot fail, loop or act are evaluated where they are bound, without a thunk. Calls with closed function arguments, such as `map (* 3) xs`, call copies of the callee in which the argument is a known function. Functions that build a constructor around a call to themselves, such as `map`, `filter` and `++`, compile to loops (tail recursion modulo cons), so a long `map` no longer uses stack proportional to its length in compiled code. Local functions that are only called become top-level functions taking their free variables, and lambdas that capture locals are specialized like closed ones. Values of type `Int`, `Float`, `Char` and `Bool` are C scalars inside function bodies (`docs/dev/typed-backend.md`). Typed primitives are inlined, comparisons in conditions compile to C comparisons, stack overflow is detected by a guard region instead of a check in every function, and only reachable code is emitted. The collector finds objects by address arithmetic instead of binary search, skips non-pointers, no longer zeroes dead objects, and collects after max(128 MiB, 2x live) instead of max(32 MiB, live), which roughly doubles peak memory.

- `Float` comparison follows IEEE in both backends: `NaN == NaN` is `False`. It was `True`, because floats were compared structurally.

- `.` with no space on either side is field access: `f.g` is no longer composition.

- Non-recursive `let` and `where` value bindings are evaluated on first use, at most once. Guards can now protect a `where` binding: `f x | x == 0 = 0 | otherwise = y where y = div 1 x`. A thunk is chosen over strictness analysis for size. It costs one allocation per evaluated binding. Effects bound by `let` run at first use.

- `Eq`, `Ord`, `Show` and `Num` are type classes, replacing the `Num` flag and structural `==`/`show`. Comparing or printing a data type requires `deriving (Eq, Ord, Show)`; without it, `A == B` is a type error. `show` follows the type, so `print (one () :: Float)` prints `1.0` and `print ""` prints `""`. A constrained type nothing fixes is an error, as in Haskell: `print []`. Local functions without a signature no longer generalize constrained type variables, so specializing a function also specializes its `where` loops; one used at two number types needs a signature.

- Literals in overloaded code are built through `fromInt`, so a polymorphic literal used as a `Float` is a `Float`. 0.1 built it as an `Int`.

- `infix`, `infixl`, `infixr`, `class`, `instance` and `do` are reserved words, and `<-` is a reserved operator.

### Added

- A CMake build (`CMakeLists.txt`), with `make` as a frontend: it configures on first use, builds in `build/` and links `./funk`. `make SANITIZE=undefined test` builds and tests funk under a sanitizer in its own directory, and `make install` installs it. `ctest` runs the test suite.

- Command line: `funk check FILE` type-checks without running; `funk eval EXPR` (`-e`) evaluates an expression; `funk repl FILE` loads a file into the REPL. `funk build` takes `-O LEVEL`, `-l LIB` and `-L DIR`. `funk help [COMMAND]`, `-h` and `--version` print to standard output. Usage errors name the problem and the help to read, and exit with status 2; failing programs exit with 1.

- `do` blocks with `p <- e`, expression and `let` statements. Each statement becomes `case e of p -> rest`, chosen over `>>` and over a strict bind primitive because both evaluate the rest inside a call: a recursive `do` loop of 20M iterations overflowed the 4 GiB stack, and now runs in constant stack. Without an `IO` type, `<-` accepts any expression. A `where` at the column of a block's items now closes that block.

- Type classes, phase 1 (`docs/dev/type-classes.md`): single-parameter `class` and `instance` declarations with superclasses, default methods and instance contexts, and `deriving (Eq, Ord, Show)`. Constraints are passed as dictionaries, chosen over monomorphization because the REPL interpreter and polymorphic recursion need code that works at any type. Calls whose dictionaries are all known call a specialized clone, `fib@Int`, so monomorphic code runs as fast as in 0.1.

```haskell
class Shape a where
  area :: a -> Float
  describe :: a -> String
  describe x = "area " ++ show (area x)
```

- Records with fields scoped to their type: `data P = P { x, y :: Float }`, construction `P { x = 1.0, y = 2.0 }`, update `p { x = 3.0 }`, patterns `P { x = a }`, and dot access `p.x`, `(.x)`. `Type.f` defines a function in a type's namespace, and `p.f` calls it with `p` first. Type-scoped fields were chosen over Haskell 98's global selectors so that two types can share a field name. The cost is that `p.x` needs `p`'s type: `getX p = p.x` needs a signature. See `docs/dev/records.md`.

- Benchmark suite (`make bench`): 10 programs in funk and in C that use the same algorithm and data structures. Each run appends timings, max RSS and build times to `bench/results.jsonl`, keyed by host and commit. `bench/bench.py report` flags regressions against an earlier run.

- `infixl`, `infixr` and `infix` declarations at top level, as in Haskell 2010. Declarations are installed by pre-scanning the module, chosen over re-associating parsed operator chains because it leaves the precedence-climbing parser unchanged. The cost is no local fixity declarations.

- Compile-time warning for a `case` or function that does not cover every constructor, naming an unmatched value. Guarded alternatives count only when a guard is `otherwise` or `True`.

### Fixed

- Type errors are reported at the sub-expression with the wrong type, not at the binding. `f :: Int -> Bool; f x = x + 1` reported `expected Int -> Bool, found Int -> Int` at the start of `f`, and now reports `expected Bool, found Int` at `x + 1`. Expected types are now pushed into lambdas, `let`, `if`, `case` and constructor arguments, so a signature's types are known before the body is inferred. Before, `m :: Int -> String; m n = case n of 0 -> "z"; _ -> n` reported `no instance Num [Char]`, because `n` had already been unified with `String`. Rigid type variables print with their names from the signature.

- Tuple constructors were registered with no type (`tydef` -1), because the type name was looked up without interning. The C backend read `tydefs[-1]` for every tuple pattern.

## [0.1.0]

### Added

- Language with Haskell syntax: clauses with pattern matching and guards, `where`, `let`, `case`, `if`, lambdas, layout rule, `data`, sections, ranges, operator definitions, `.`, `|>`, `>>>` and `$`.

- Hindley-Milner inference with optional, rigid signatures. Numbers use a single `Num a` constraint over `Int` and `Float` with polymorphic literals, chosen over type classes for size; see `docs/dev/design.md`.

- Strict evaluation with effects sequenced by `>>`.

- REPL (`funk`) and interpreter (`funk FILE`).

- Compiler to standalone C (`funk build`, `funk emit-c`).

- Shared runtime with a conservative mark-sweep GC and a 4 GiB lazily committed stack.

- Prelude of 78 functions over lists, `Maybe`, `Either`, characters and strings.

- Test suite (`make test`): every run test is checked under both backends.
