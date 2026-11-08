# Controls in

How values from outside a pattern reach it: Tidal's `cF`, `cI`, `cS` and the rest, `in0`..`in127`, `sec` and `msec`, and the `trigger` family. Group 4 of [combinators.md](combinators.md#scope). Decided 2026-11-08.

## Tidal's model

From tidal-core 1.10.2 and tidal 1.10.3:

- A query's `State` holds its arc and a map of controls (`Pattern.hs`). `cF d name` reads `name` from the map at each query, or `d` if absent (`Core.hs`); `in0` is `cF 0 "0"`.
- The stream's listener sets a control from OSC `/ctrl name value`; an `Int32` name stands for its decimal string, and a value is a float, int or string (`Stream/Listen.hs`).
- The stream adds `_cps`, the tempo, to every query, and `_t_pattern`, the cycle a pattern was last evaluated at, to that pattern's queries (`Stream/Process.hs`). `trigger` and its kind shift a pattern by `_t_pattern`; `sec` and `msec` scale by `_cps`.

## Decisions

1. **The query carries the controls.** `Pattern a` holds a function of a `State`, an arc and a list of named values, as Tidal's does, and every combinator passes it on. A query stays pure. The alternative, `cF` reading a table the host fills before each query, needs no change to the combinators, but makes a query depend on when it runs. `p.query pat span` stays, with no controls.
2. **Names, by source.** All three can be on at once, since their names do not overlap:
   - OSC: Tidal's `/ctrl name value`, under its own name.
   - MIDI CC: `"cc21"` for controller 21 on any channel, and `"1:cc21"` for channel 1. Values are 0 to 1, as CC 0 to 127.
   - Plugin parameters: `"p1"` to `"p16"`, 0 to 1, which a DAW can automate.
3. **Choosing a source.** A top-level `controls`, which the hosts read as they read `pat`: `controls = [c.osc 6010, c.midiCC, c.params]`. Without one, no source is on. The plugin may add a menu for the same choice later.

## Cost

A query builds a `State` at each combinator. 2001 cycles of `every 3 (fast 2) (off 0.125 (+ 7) (chunk 4 (fast 2) (mini ...)))` took 1.39 s in the interpreter, against 1.24 s before, so about 12% more (3 runs each, 2026-11-08).

## Steps

1. Done 2026-11-08, all 20 cases of `lib/tidal` matching. `lib/pattern.fk`: `State`; the controls `cF`, `cN`, `cI`, `cB`, `cR`, `cT`, `cS` and their `_` and `0` forms, `in0`..`in127`, `sec`, `msec`, and `trigger`, `qtrigger`, `ctrigger`, `rtrigger`, `ftrigger`, `mtrigger`, `triggerWith`. Tested against Tidal (`lib/tidal`).
2. Done 2026-11-08. The hosts query with a table they own (`projects/funkgraph/controls.h`), which `step` reads through `p.readControls`: `_cps`, and `_t_pattern`, the cycle of the window `pat` changes at. The plugin's clips get 0, so a clip plays from its own cycle 0. Tests: `hosts/midi` (`trigger`, `qtrigger`, `sec`) and `hosts/clap` (`sec`).
3. Done 2026-11-08. `controls`, a list of `p.osc port`, `p.midiCC` and `p.params`, chooses the sources; defining it again replaces them, and clears their values. OSC is `projects/funkgraph/controls.c`, polled without blocking before each `step`, so it needs no thread. A CC from the plugin's MIDI input, or from `funk-midi --in` through libremidi, waits in a table of 16 * 128 for the next `step`; `funk-midi`'s `:cc` sets one by hand. The plugin's parameters are CLAP's `p1` to `p16`. Tests: `kernels/controls` (the OSC parser over loopback), `hosts/midi` (`:cc`) and `hosts/clap` (OSC, a CC and a parameter).

## Limits

- The OSC socket binds 127.0.0.1 only, as Tidal's listener does by default. Two plugin instances cannot share a port: the second reports it.
- The plugin's state does not hold its parameters' values; a host's automation restores them.
- `funk-midi`'s MIDI input needs a build with libremidi; checked by hand, CC 21 from one `funk-midi` to another's virtual input, as no CI job builds it.
