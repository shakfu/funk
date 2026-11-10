# Pattern slots

Tidal's slots and their commands, R1 and R2 in [tidal-comparison.md](tidal-comparison.md): `d1 $ s "bd*2"` and `p "name" $ ...` replace what one slot plays, the host plays every slot at once, and `hush`, `mute`, `solo`, `once` and the rest act on them. `lib/stream.fk` follows Tidal 1.10.3's `Stream/UI.hs`, `Stream/Process.hs` (`updatePattern`, `playStack`, `setPreviousPatternOrSilence`), `Stream/Listen.hs` and `Boot.hs`. funk-midi has them; the plugin does not yet.

## Semantics

- **IDs** are Tidal's: an `ID` is a string, with `Num` and `FromString` instances, so `p 1` and `p "1"` are one slot.
- **A replace** adds `_id_` (`# pS "_id_"`) and `_t_pattern` to the pattern, as `updatePattern` does, and plays from the next window. `_t_pattern` is the cycle of that window, as `pat`'s was ([controls-in.md](controls-in.md)); Tidal's is the cycle at evaluation.
- **A pattern that fails at once** replaces nothing. It is queried over `Span 0 0` first, as Tidal forces `queryArc pat (Arc 0 0)`.
- **A window that fails** plays nothing, and each slot then plays its previous pattern, as Tidal's `doTick` and `setPreviousPatternOrSilence` do. A slot given once falls silent. Tidal's history keeps each pattern as given, so a slot that falls back to an earlier one sends no `_id_`; funk keeps this.
- **Order**: the slots stack in order of ID as strings, as `Map.elems` gives them: "1" < "10" < "2".
- **`d1` to `d12`** add `orbit 0` to `11` with `|<`, so a pattern's own orbit wins; `d13` to `d16` add none.
- **`pat`** of `Controls` is given to `d1`. A `Pattern Int` `pat` plays as before, beside the slots.

## Commands

As Boot.hs's: `hush`, `list`, `mute`, `unmute`, `unmuteAll`, `unsoloAll`, `solo`, `unsolo`, `once`, `asap`, `first`, `panic`, `only`, and `streamAll` for Tidal's `all`.

- A slot's state is Tidal's `PlayState`: its pattern, `psMute`, `psSolo` and its history. While any slot is soloed, only soloed slots play.
- `first pat` plays cycle 0 of `pat` once, from the next window, as Tidal's `onSingleTick` does from now. `once` plays a random cycle, 0 to 8192, the same way. Neither is a slot: they send no `_id_`, and mute and solo pass them by.
- `streamAll f` applies `f` to every slot and to each pattern played once, from the next window. Tidal's name, `all`, is the prelude's; in Tidal 1.10.3 it is ambiguous at the prompt too.
- `only` takes `\_ -> action`: `only (\_ -> d1 $ s "bd")`. funk is strict, so Tidal's `only $ d1 ...` would run `d1` before `hush`; in funk it is a type error.
- An action input runs at the REPL, as in GHCi, so `hush` alone hushes ([design.md](design.md#effects-effect-arrows)).
- Tidal's listener's playback commands arrive on the `controls` OSC ports: `/mute`, `/unmute`, `/solo`, `/unsolo` and `/silence` with an ID, a string or an int; `/muteAll`, `/unmuteAll`, `/unsoloAll` and `/hush`. Others are reported, as Tidal's are. `:osc ADDR ARG` in a script gives one as if it came in.

## Tempo

As Boot.hs's and its clock's (tidal-link `Clock.hs`), from Tidal's default of 0.575 cycles a second, 138 bpm at 4 beats a cycle.

- An onset with a `cps` sets the tempo from its start, as `processCps` does; the rest of its window keeps the old tempo, as the rest of Tidal's tick does. `setcps` is Boot's `once . cps`, so it acts the same way, a lookahead later.
- `setbpm`, OSC `/setcps` and `/setbpm` set the tempo from the next window. Every tempo is clamped to Link's 20 to 999 bpm.
- `setCycle c` starts cycle `c` at the next window; `resetCycles` is `setCycle 0`.
- `nudgeAll n` shifts the slots, not the patterns played once, by `n / cps` cycles, as Tidal's `doTick` does: seconds over cycles a second, so its delay changes with the tempo.
- `getcps`, `getbpm` and `getnow` return `Float`s; Tidal's return `Rational`s. `getnow` is the cycle now, as Tidal's `getCycleTime`.
- The host keeps the tempo exactly and the cycle's length in microseconds to the nearest; at 0.575 that drifts 0.25 ppm.
- The listener's `/setcps`, `/setbpm` and `/setCycle` take a float, `/nudgeAll` a double, as Tidal's do; `/resetCycles` takes any. In a script, `:osc /nudgeAll 0.25d` gives a double.

## Transitions

As Boot.hs's and `Transition.hs`'s: `xfade`, `xfadeIn`, `histpan`, `wait`, `waitT`, `jump`, `jumpIn`, `jumpIn'`, `jumpMod`, `jumpMod'`, `mortal`, `interpolate`, `interpolateIn`, `clutch`, `clutchIn`, `anticipate`, `anticipateIn` and `forId`, and the mappers they use, `_xfadeIn` to `_mortalOverlay`, for `waitT`.

- A transition adds its pattern to the slot's history and plays the mapper of the history, from the cycle the next window starts at. As in Tidal, that pattern has no `_id_`, and a new slot's history starts with `silence`. `forId` passes its pattern to the mapper only.
- Times are `Float` cycles, rounded to ticks. `anticipate`'s echoes fall at step times read from an envelope, which Tidal keeps exact; rounded, some land a window apart, so `lib/tidal-dirt` does not check it.

## Host side

The slots are a C table of `StablePtr`s to `PlayState`s, by ID (`projects/funk-midi/stream.c`), with the patterns played once and `streamAll`'s function. Commands replace a slot's `StablePtr` and free the old one. Each window, `slots`, compiled once at start-up, reads the table through `playStack`. The imports are linked in as glue (`projects/funk-midi/glue.fk`), so no C compiler runs.

A table in funk would need a mutable reference, which funk lacks; a stable pointer is already a GC root.

A value named `p` beside `import pattern as p` needed a change to module scope ([modules.md](modules.md)).

## Tests

- `lib/tidal-dirt`, case `slots`: 5 slots through Tidal's own `playStack`, against funk-midi's; all 40 messages match.
- `hosts/slots`: order, orbits, replacing, `_d2`, `d10_`, `p 13`, `p "3"`, a pattern that fails at once, one that fails later, and `trigger` from its slot's time.
- `hosts/slots-midi`: two slots to MIDI, beside a `Pattern Int` `pat`.
- `hosts/commands`: each command, and the OSC ones.
- `lib/tidal-dirt`, cases `muted` and `soloed`: `PlayState`s with mute and solo through Tidal's `playStack`.
- `repl/actions`: an action input runs.
- `hosts/tempo`: each tempo command and its OSC form; `hosts/slots-midi`: an event's `cps` to MIDI.
- `lib/tidal-dirt`, cases `xfade` to `forId`: 12 transitions at cycle 0 through Tidal's own mappers, queried a window at a time as funk-midi queries; all 229 messages match.
- `hosts/transitions`: transitions past cycle 0, on a new slot, on a muted one, and `anticipateIn`.

## Not done

- Slots in the plugin.
