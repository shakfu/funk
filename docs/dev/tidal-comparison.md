# Tidal comparison

Every known difference between funk and Tidal, and every gap. Written 2026-11-09.

Rule: funk matches Tidal and SuperDirt. A difference stays only for a stated reason: a better design, a fix of a Tidal defect, or a constraint of real-time hosts. Otherwise it is a gap to close.

References, checked from source, not documentation:

- tidal 1.10.3 and tidal-core 1.10.2, from Hackage (`~/.cabal/packages/hackage.haskell.org/`). Exports were checked with `ghci -package tidal` against `Sound.Tidal.Context`.
- SuperDirt 1.7.3, commit `7e245e8` (2022-01-04), as installed in `~/Library/Application Support/SuperCollider/downloaded-quarks/SuperDirt`. It is not checked whether this is the release Tidal 1.10.3 expects.

Status:

- **Gap**: no reason; to close.
- **Kept**: a reason given; the difference stays.
- **Review**: a reason is missing or weak; decide.

## Summary

| Area | Gaps | Kept | Review |
|-|-|-|-|
| Language and literals | 3 | 2 | 1 |
| Time | 2 | 1 | 0 |
| Pattern functions | 8 | 1 | 2 |
| Mini-notation | 3 | 2 | 0 |
| Controls | 2 | 0 | 0 |
| Runtime and workflow | 6 | 1 | 0 |
| SuperDirt sound | 8 | 0 | 1 |

L1, L2 and L4 are done: string literals are patterns, controls take patterns, and patterns divide. The largest remaining gap is the runtime (R6).

## Language and literals

**L1. Done 2026-11-09: a string literal is a pattern.** `class FromString` in the prelude, as `FromFloat`: a literal at another type calls its `fromString`, and an unfixed one is a `String`. `instance Parseable a => FromString (Pattern a)` parses it as Tidal's `parseBP_E` does, for `Int`, `Float`, `String`, `Note` and `Bool`. `lib/tidal` checks 7 cases of literals against Tidal. Generalization follows GHC's, done 2026-11-09 ([design.md](design.md#monomorphism-restriction)):

- In a file, `riff = "0 2"` takes its type from its uses, under GHC's monomorphism restriction. A REPL or host input generalizes it, as GHCi's prompt does, so `riff :: FromString a => a` plays as `n riff` and `s riff` in later inputs, and `pat = p.fast 2 "0 2"` plays in a host as a `Pattern Int`.
- Unlike GHCi, a generalized value is computed once per type it is used at, not at every use, and once at its default types (`Int`, `Float`, `String`) as it is defined, so its errors show then.

**L2. Done 2026-11-09: controls take patterns.** Each of the 257 controls takes Tidal's type: `Pattern Float` for Tidal's `Double`, `Pattern Int`, `Pattern String`, and `Pattern Note` for `n`, `note`, `up` and `number`. `Note` is Tidal's: a `Float` whose mini-notation reads note names first, with Tidal's `Show`. Tidal's signals and `irand`, `run` and `scan` are polymorphic, as Tidal's are, so `n (irand 8)` and `n (segment 4 (range 0 7 sine))` check. 9 cases in `lib/tidal` check them against Tidal. A signal's type needs `FromFloat` where Tidal's needs `Fractional` (L4), and `run` and `scan` need `Num` and `Ord` where Tidal's need `Enum` (L8).

**L3. Done 2026-11-10, for patterns alone: `Functor`, `Applicative` and `Monad` names.** `lib/pattern.fk` has `fmap`, `<$>`, `<$`, `pure`, `<*>`, `liftA2`, `return` and `>>=` on `Pattern` only, and Tidal's `<*`, `*>` and `<<*`, with Tidal's and base's fixities. `>>=` is Tidal's `unwrap`. `lib/tidal` checks 12 cases against Tidal. Kept difference: they take no list or `Maybe`, as funk has no higher-kinded classes ([type-classes.md](type-classes.md#later-phases)). When it has them, these become instances, and code that uses them stays as it is.

**L4. Done 2026-11-09: `Fractional`.** `class (Num a, FromFloat a) => Fractional a`, with `/` and `recip` and Haskell's defaults for each. `Pattern a`, `Note` and `Controls` are instances, as Tidal's `Pattern a`, `Note` and `ValueMap` are, so `(26/8) <~ p` ports as written; a float literal at `Controls` is a `speed`. `lib/tidal` checks 7 cases against Tidal. The C emitted for the 6 benchmarks that divide is unchanged but for constructor numbers. Differences, kept:

- A float literal needs `FromFloat`, Haskell's needs `Fractional`. They differ only for a type with `FromFloat` and no `/`, such as `lib/dsp.fk`'s `Sig`, which no Tidal code has.
- No `fromRational`: it waits for T3's `Rational`. `Floating` (`pi`, `sin` on patterns) is not done.

**L5. Done 2026-11-10: the pattern library is unqualified in the hosts.** funk-midi and the plugin import `pattern` as `p` and with `(..)`, so Tidal's names work as written and `p.` names still do. No `pattern` name is the prelude's. One clashed with `control`: `pattern`'s internal `clip`, now `clipEvent`, so `clip` is Tidal's control. A name defined at the prompt shadows an imported one, as at GHCi's ([modules.md](modules.md)); funk-midi's `sig` needs that. `pattern` has no export list, so its 673 names, helpers included, are all in scope. [patch-census.md](patch-census.md) ran the 22 patches this way.

**L8. Gap: no `Enum` class.** `enumFromTo` is `(Num a, Ord a)`, so `run`'s ranges follow `Int`'s rule at every type: Tidal's `run 3.5`, `[0 .. 2.5]`, ends at 3 by Haskell's `b + 1/2` rule for fractional types; funk's ends at 2. Ranges over `Char` and user types, `succ`, `pred`, `toEnum`, `fromEnum` and `deriving Enum` are missing too. Cost: `[a..b]` desugars to `enumFromTo` by name (`parse.c`), and fusion recognizes it by name (`fuse.c`); as a method it elaborates to an instance call that fusion must learn to match, or fused loops slow down. Do after L4.

**L9. Done 2026-11-11: `RealFrac`.** `class (Fractional a, Ord a) => RealFrac a` has `floor`, `ceiling`, `round` and `truncate`, for `Float` and `Note`, so `quantise` and its kin take any `RealFrac`, as Tidal's do. The results are `Int`s, where Haskell's are any `Integral`: funk's `Integral` cannot make a value. `chromaticiseBy` takes any `(Num a, Ord a)`, as Tidal's `(Num a, Enum a, Ord a)`. `bench.py ab`: +0.1% and -0.6% geomean.

**L10. Done 2026-11-11, but `Monoid`: `Semigroup`.** The prelude has `class Semigroup a` with `<>`, infixr 6, for `String` and lists, and `<>` on patterns overlays, as Tidal's instance. `Monoid`'s `mempty` is a constant method, which funk's classes do not have ([type-classes.md](type-classes.md)), so Tidal's `mempty = silence` is missing.

**L6. Kept: `|>` is the prelude's pipe.** Tidal's `|>` is `#`, and funk has `#`. Of Tidal's 264 `d1 $` doc examples, 108 use `#` and 1 uses `|>` ([doc-census.md](doc-census.md)).

**L7. Review: a mini-notation typo fails at definition, an unknown chord name too.** Tidal's `parseBP_E` raises its error when the pattern is queried, and an unknown chord name plays as its root ([music.md](../music.md#mini-notation)). funk reports both when the definition is evaluated, and the playing pattern goes on. Errors are reported, not played wrong. Keep this, unless a Tidal patch depends on the root fallback.

## Time

**T1. Kept, with a residue: time is `Int` ticks, not `Rational`.** Arithmetic stays bounded, with no bignum allocation on the query path. The rate is 720720 ticks per cycle, so a cycle divides exactly into 16ths but not 32nds: 1604 of 6456 ported events round by up to 30 ticks ([patch-port.md](patch-port.md#rounding)). At 46126080 (720720 * 64), every event except one test's random values is exact. Denominators with a prime above 13, or a power past those, still round: `iter 17`, for one. Recommendation: move to 46126080. Ticks reach Int64's limit at about 2e11 cycles, fewer under `_fast`.

**T2. Done 2026-11-10: speed factors are `Float`s.** `fast`, `slow`, `ply`, `segment`, `hurry`, `loopAt`, `smash`, `fastGap`, `fastSqueeze` and 9 more take a `Pattern Float`, as Tidal's take a `Pattern Time`, so `slow "<1.5 0.75>"` and `density` of a continuous pattern port as written. Each factor is its nearest ratio with a denominator up to 65536, applied exactly in ticks ([combinators.md](combinators.md#differences-from-tidal)). `lib/tidal` checks 17 cases against Tidal. Kept difference: a mini-notation decimal such as `"0.6"` is 3/5, as Tidal's literal `0.6` is, where Tidal's parser keeps the `Double`'s binary fraction. On `slow "<1.5 0.6>"` that moves events by 1 tick and keeps an onset Tidal loses at a cycle boundary. A constant factor costs an `innerJoin`, as in Tidal: `p11`'s queries take 21% longer than with `_fastR 5 3` (F8 would remove it).

**T3. Gap: times are `Float`s.** A time argument is a `Pattern Float`, rounded to a tick. `toRational <$>` is dropped in ports. This follows from T1 and L4. A `Rational` literal type would remove the rounding before the tick.

## Pattern functions

Coverage of `Core.hs`, `UI.hs` and `Control.hs` is complete: 178 cases in `lib/tidal` match ([combinators.md](combinators.md#status)). The functions inside block comments there are not exported by Tidal 1.10.3 either (checked: `preplace`, `fill`, `pequal`, `scrumple`, `toMIDI`, `striateL`, `en`, `<<~`, `<~>`). They are not gaps.

**F1. Done 2026-11-11: `Scales.hs`.** `scale`, `scaleList`, `scaleTable` with Tidal's 76 scales, `getScale`, `scaleWith`, `scaleWithList`, `raiseDegree(s)` and `lowerDegree(s)`, in `lib/pattern.fk`. As in Tidal, the scales are not names of their own. `lib/tidal` checks every scale against Tidal.

**F2. Gap: `Time.hs` functions are not exported.** `sam`, `nextSam`, `cyclePos`, `toTime`, `fromTime`, `hull`, `subArc`, `arcCycles`, `arcCyclesZW`, `cycleArc` and `isIn`. funk has `cycleOf` for its own use. User-written combinators call these.

**F3. Gap: `Pattern.hs` functions Tidal exports.** `queryArc` exists as `p.query`, and `filterValues` exists. Missing:

- `withEvent`, `withEvents`, `withValue`, `withPart`, `withQueryTime`, `withResultTime` and the rest of `withQuery*` and `withResult*`;
- `filterEvents`, `splitQueries`, `collect`, `collectBy`, `empty`, `revv`, `pace`;
- `wholeStart`, `wholeStop`, `eventPart*`, `eventHasOnset`, `isAnalog`, `isDigital`;
- the `Value` accessors `getI`, `getF`, `getS`, `getB`, `getR` and `extract*`.

The rest, such as `patternify*`, `applyPatToPat*` and `matchManyToOne`, are internal; port them as needed.

**F4. Done 2026-11-11: `Simple.hs`.** `crunch`, `scratch`, `louder`, `quieter`, `silent`, `skip`, `left`, `right`, `higher`, `lower`, `faster` and `slower`, in `lib/control.fk`. A string literal as a control pattern is a `sound`, as Tidal's `IsString ControlPattern`: `"bd*2 sn" # gain 0.8`. `control.fk`'s helpers `left` and `right` are now `onLeft` and `onRight`. `lib/tidal` checks each against Tidal; `scratch` through `rev (chop 16 ...)`, since `chop 32`'s 32nds round (T1).

**F5. Gap: `Show.hs`.** `draw`, `drawLine`, `drawLineSz`, `showAll` and `stepcount`. Tidal users read `drawLine "bd*2 sn"` at the REPL to check a pattern. funk-midi prints events but has no text drawing.

**F6. Gap: steps.** Tidal 1.10's step counts (`setSteps`, `withSteps`, `keepSteps`) and the exported functions of `Stepwise.hs`: `stepcat`, `stepalt`, `steptake`, `stepdrop`, `stepzip`, `stepJoin`, `expand`, `contract` and `extend`. [combinators.md](combinators.md#differences-from-tidal) lists it as not ported, without a reason.

**F7. Gap: `chordList`.** Tidal's `chordList` lists the chord names as a string; funk has `chordTable`. Chords.hs's named chords, such as `major`, are not exported by `Sound.Tidal.Context` (checked), so they are not a gap.

**F8. Gap: no `pureValue`.** Tidal 1.10 applies a constant argument directly, where funk joins with an `atom`. An event crossing a cycle boundary then comes in two parts, with the same onset ([combinators.md](combinators.md#differences-from-tidal)). It is inaudible in the hosts, which play onsets. A part count or a `drawLine` would show it. It is a cost, not a design.

**F9. Kept: `substruct'`, and so `stripe`, clip events to the query.** Tidal's returns each event over its whole span. Any host that queries in blocks would then play an onset once per block it overlaps. This fixes a Tidal defect, assuming Tidal's own scheduler, which queries per frame, has the same defect (inference; not tested).

**F10. Review: `rot` orders a cycle's events by whole, then part.** Tidal puts each event's source position first. No reason is recorded. The cases in `lib/tidal` match, so the difference lies outside them. Find a case where the two orders differ, then follow Tidal.

**F11. Review: `euclid` keeps `p.euclid 3 8 36`.** This works because `36` is an atom, and Tidal's types agree. It is no longer a difference; drop it from [combinators.md](combinators.md#differences-from-tidal).

## Mini-notation

**M1. Done 2026-11-11: `Bool` mini-notation.** `t` and `1` are `True`, `f` and `0` are `False`, one character each, as Tidal's `pBool`. A euclid negates its off steps, as `euclidOffBool` does. `lib/tidal` checks 25 cases against Tidal, and 8 of the 9 doc examples it blocked now match ([doc-census.md](doc-census.md)).

**M2. Gap: no `^name`.** Tidal's `pVar` reads a control in mini-notation through the type's `getControl`, such as `cF_` and `cS_`. [music.md](../music.md#mini-notation) says it is missing "since there are no control inputs". That reason is stale: `cF` and the rest exist since 2026-11-08 ([controls-in.md](controls-in.md)).

**M3. Gap: no `Char` or `Rational` parser.** Tidal has `Parseable` for `Char`, `Rational` and `ColourD`. funk has `Int`, `Float`, `Note` and `String`. `Rational` follows T3. `ColourD` is for Tidal's visuals; port it only if they are ported.

**M4. Kept: parse at definition.** See L7.

**M5. Kept: two modifiers on a step.** funk parses `a*2/5 b` and `a/5*6 b`. Tidal 1.10.2 rejects them when more of the sequence follows, and misplays them at the end ([patch-port.md](patch-port.md#in-tidal-1102)). This fixes a Tidal parser defect. A Tidal string that parses plays the same in both.

## Controls

**C1. Done: controls of patterns.** See L2.

**C2. Done 2026-11-10 in funk-midi: an event's `cps` sets the tempo**, at its onset, as in Tidal's `Stream/Process.hs:151`, so `# cps "<0.5 1>"` changes speed ([stream.md](stream.md#tempo)). In the plugin the DAW owns the tempo (R7).

**C3. Gap: the rest of `Params.hs`.** funk has the 257 base controls. Tidal's `Params.hs` also generates, for most of them:

- `xCount`, `xCountTo` and `xTake` (189, 189 and 195): counters and lists that keep state across events, through `pStateF` and `pStateList`;
- `xbus` and `xrecv` (246 and 208): SuperDirt control buses, which need S2's buses.

Also missing are `array`, `pB`, `pR`, `pX` and the `pState*` makers. That is 1035 names in total. `midinote`, `drum`, `drumN` and `grain'` are done, 2026-11-11.

## Runtime and workflow

`Boot.hs` and `Stream/UI.hs` hold Tidal's runtime. funk-midi has Tidal's slots, `:stop` and `:play`, and `:cycle MS`.

**R1. Done 2026-11-10: named pattern slots.** `d1` to `d16`, `p "name"`, `_p`, `p_`, `_d1` and `d1_`, in `lib/stream.fk`, in funk-midi; `pat` of `Controls` is `d1`'s. `lib/tidal-dirt` checks 5 slots against Tidal's `playStack` ([stream.md](stream.md)). Not yet in the plugin.

**R2. Done 2026-11-10: slot commands.** Boot.hs's `hush`, `once`, `first`, `asap`, `solo`, `unsolo`, `mute`, `unmute`, `unmuteAll`, `unsoloAll`, `list`, `panic` and `only`, and the listener's OSC `/silence`, `/muteAll` and the rest ([stream.md](stream.md#commands)). Kept differences: Tidal's `all` is `streamAll`, as the prelude's `all` keeps its name; `only` takes `\_ -> action`, as funk is strict.

**R3. Done 2026-11-10 in funk-midi: tempo and cycle commands.** `setcps`, `setbpm`, `getcps`, `getbpm`, `getnow`, `resetCycles`, `setCycle` and `nudgeAll`, and the listener's OSC forms ([stream.md](stream.md#tempo)). The default is Tidal's 0.575 cycles per second: its clock's `defaultCps` (tidal-link `Clock.hs`), not the 0.5625 of a doc comment (`Safe/Boot.hs:75`). Kept difference: the getters return `Float`, as funk has no `Rational` (T3). In the plugin the host's tempo stays authoritative (R7).

**R4. Done 2026-11-10 in funk-midi: transitions.** Boot.hs's `xfade` to `forId`, and `Transition.hs`'s mappers, `_xfadeIn` to `_mortalOverlay` ([stream.md](stream.md#transitions)). `lib/tidal-dirt` checks 12 against Tidal's own. `wash`, `washIn` and `mortalOverlay`, which Boot.hs hides, are `_wash`, `_washIn` and `_mortalOverlay`.

**R5. Done 2026-11-10 in funk-midi: state set from code.** `setF`, `setI`, `setS`, `setR`, `setB` and `getState`; `cF` and the rest query a value that is a pattern, as Tidal's `VPattern` ([stream.md](stream.md#state)). `lib/tidal` checks 9 cases against Tidal. Kept difference: `setR` takes `Float`s (T3).

**R6. Gap: Ableton Link and editor integration.** Missing are `enableLink` and `disableLink`. Tidal's editor plugins, for Pulsar, VS Code, Vim and Emacs, send blocks to GHCi on standard input. funk-midi reads standard input, and the plugin takes code over UDP (`projects/clap/udp_server.cpp`). No editor package targets either. It is not checked whether an existing Tidal editor plugin can be pointed at `funk-midi` unchanged.

**R7. Kept: in the plugin, the DAW owns the tempo and transport.** A cycle is 4 beats at the host's tempo ([music.md](../music.md)). Tidal's own clock would drift from the host's. Tidal's `cBeatsPerCycle` of 4 agrees with this.

## SuperDirt sound

`d.kit` plays samples as `dirt_sample` and `dirt_gate` do: `begin`, `end`, `speed`, `loop`, `accelerate`, `sustain` or `legato`, `note`, `octave`, `unit "c"`, `cut` and the fades ([delivery.md](delivery.md)). The MIDI target follows `DirtEventTypes.sc`. The rest is missing.

**S1. Gap: per-event effects.** All 13 modules of `core-modules.scd`: `vowel`, `shape`, `hpf`, `bpf`, `crush`, `coarse`, `lpf`, `pshift`, `envelope`, `grenvelo`, `tremolo` and `phaser` (`sound` is done). Their controls exist in `lib/control.fk` and are ignored by `d.kit` (inference: no use of them in `lib/dsp.fk` or `lib/kernels.fk`).

**S2. Gap: orbits and global effects.** Each orbit has `dirt_delay`, `dirt_reverb`, `dirt_leslie`, `dirt_rms` and `dirt_monitor` with its limiter (`DirtOrbit.sc:59-65`). funk has no orbits; `orbit` reaches `d.kit` but routes nothing.

**S3. Gap: synths.** 11 SynthDefs in `synths/default-synths.scd` and 33 in `library/default-synths-extra.scd`, from `superpiano` to `superfm`, `soskick` and `sossnare`. Also the 19 SynthDefs of `library/default-effects-extra.scd`, such as `squiz`, `waveloss`, `triode`, `krush`, `octer`, `ring`, `distort` and the `spectral` set. `s "superpiano"` plays nothing in funk.

**S4. Gap: the pan law.** Tidal's `pan` runs from 0 to 1. SuperDirt maps it to -1 to 1 and pans a mono sample with `Pan2` and a stereo one with `Balance2` (`SuperDirtUGens.sc`, `DirtPanBalance2`); `Pan2` is equal-power. `d.kit` is linear, with gain 1 on both sides at 0.5 (`lib/dsp.fk:168`). A centred mono sample is then about 3 dB louder than in SuperDirt (inference from `Pan2`'s equal-power law; not measured). Port `Pan2` and `Balance2`, and `DirtSplayAz` for more than 2 channels.

**S5. Gap: cubic interpolation.** `dirt_sample` reads with `BufRd` at `interpolation: 4`, cubic (`core-synths.scd:47`). `d.kit` is linear ([music.md](../music.md#control-patterns)).

**S6. Gap: `unit "s"`** plays as `unit "r"` ([music.md](../music.md#control-patterns)).

**S7. Gap: file formats.** SuperDirt loads `wav`, `aif`, `aiff` and `aifc` (`DirtSoundLibrary.sc:14`). funk loads WAV only.

**S8. Gap: the UGens beneath.** Exact parity means porting SuperCollider's C++ for the 50 distinct UGens these synths use, plus SuperDirt's 10 `Dirt*` classes. These include `RLPF`, `RHPF`, `HPF`, `BPF`, `Formlet`, `CombL`, `AllpassN`, `AllpassL`, `DelayC`, `PitchShift`, `Limiter`, `LeakDC`, `EnvGen`, `Pan2`, `Balance2` and `BufRd`. Control-rate inputs follow scsynth: one value per 64-sample block, interpolated as each UGen does. Two choices remain open:

- Random UGens (`PitchShift`, `LFNoise1`) draw from scsynth's per-synth RNG. Port the RNG and its seeding for exact output, or match only the statistics.
- scsynth computes in `float32`. Match it bit for bit in `float32`, or work in double with a stated error bound.

**S9. Done 2026-11-09: an OSC target.** `funk-midi --dirt [HOST:]PORT` sends `/dirt/play` bundles as Tidal's `superdirtTarget` does ([dirt.md](dirt.md)). `lib/tidal-dirt` checks 122 messages of 12 patterns against Tidal's own `toOSC`, float bits included. S1 to S8 now matter only in the plugin and without SuperCollider. It shakes hands as Tidal does. An event's `cps` sets the tempo (C2).

**S10. Review: 4 voices a name.** SuperDirt starts a synth per event, without a limit. `d.kit` gives each name 4 voices, so a fifth overlapping event takes the oldest's voice, which is audible. The reason holds: the graph is fixed while it plays, and the audio thread cannot allocate ([delivery.md](delivery.md)). The number is the open part. Pick a default no ported patch exceeds, and report a stolen voice.

## Method

- Names: every top-level signature in tidal-core's `src/Sound/Tidal/*.hs`, `Params.hs` included, less those defined in `lib/*.fk`. Each remaining name was then checked with `ghci -package tidal` (`:t Sound.Tidal.Context.<name>`) for export. Parser internals of `ParseBP.hs` and the helpers of `Utils.hs` are left out.
- Behaviour: from [combinators.md](combinators.md), [patch-port.md](patch-port.md), [delivery.md](delivery.md), [controls-in.md](controls-in.md) and [music.md](../music.md).
- SuperDirt: `synths/*.scd`, `library/*.scd` and `classes/*.sc`, grepped for SynthDefs and UGens.

Not yet checked:

- the Tidal API beyond `tidal-core` and `Boot.hs`, such as `Sound.Tidal.Stream`'s target configuration and its OSC formats other than SuperDirt's;
- SuperDirt's event-level parameters one by one, beyond those named above;
- whether the installed SuperDirt 1.7.3 is the release that matches Tidal 1.10.3.
