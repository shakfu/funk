# A SuperDirt target

`funk-midi --dirt [HOST:]PORT` sends the slots, `d1` and the rest, to SuperDirt as Tidal 1.10.3's `superdirtTarget` does, in place of MIDI. This closes S9 in [tidal-comparison.md](tidal-comparison.md): anyone running SuperCollider hears SuperDirt itself.

## What is sent

One OSC bundle per onset whose controls name `s`, Tidal's `superdirtShape` (`Stream/Target.hs`). The bundle holds one `/dirt/play` message of name and value pairs, in name order, as Tidal's `Map.toList` gives them (`Stream/Process.hs`, `toOSC`):

- the event's controls: a `Float` as an OSC `float32`, an `Int` as an `int32`, a `String` as a string;
- `cps`, `cycle` (the onset's cycle) and `delta` (the event's length in seconds), unless the pattern sets them: Tidal's `Map.union` keeps the pattern's;
- `_id_`, the slot's ID, which each slot adds ([stream.md](stream.md)).

`lib/control.fk`'s `writeDirtWith` writes each event's times, cycle and controls to the host's buffer; `projects/funk-midi/dirt.c` encodes the bundle. The encoding stays in C because funk has no way to write a big-endian `float32`.

An event's start and stop are rounded to the nearest microsecond, and `delta` is their difference, as Tidal's Link times are. That Link rounds to the nearest (`llround`) is assumed, not checked against Link's source.

## When it is sent

The bundle's time tag is the onset's wall time plus its `nudge`, and SuperDirt plays it at that time: its `playFunc` takes `time - thisThread.seconds` as the event's latency. Tidal stamps its bundles at the onset's Link time less `oLatency`, 0.2 s, but its Link timeline runs `clockProcessAhead`, 0.3 s, ahead of the query, so its bundles reach SuperDirt about 0.1 s before they sound. funk-midi's clock has no such offset: an onset's time is when it sounds. So `--dirt` raises the lookahead from 50 ms to 0.1 s, and bundles go out as each window is computed. On a local listener, 12 bundles arrived 97.7 to 99.6 ms before their time tags.

Cycle 0 starts one lookahead after start-up; otherwise the first window's bundles would be late. A redefinition plays from the first window past the lookahead, so an edit sounds 0.1 s later than with MIDI, as in Tidal.

## The handshake

As Tidal's (`Stream/Target.hs`, `handshake`): `/dirt/handshake`, then 2 s of listening; while SuperDirt has not replied, again every 2 s, with "Waiting for SuperDirt (v.1.7.2 or higher).." once; on `/dirt/handshake/reply`, "Connected to SuperDirt." once; on `/dirt/hello`, another handshake. SuperDirt 1.7.3 never sends `/dirt/hello`; it only answers one (`SuperDirt.sc`, `connect`). So a SuperDirt started again is not shaken again, in Tidal or here. Tidal reads the reply for control bus indices, for `^name` controls; funk has none, so it ignores the reply's data. Without it, a SuperDirt that never started, as when its server fails to boot, would go unreported, since UDP has no error for a port nobody reads. Until the reply, a send error is not printed: with no `sclang` at all, the OS refuses each datagram, and the waiting message says so once.

## Not done

- An event's `cps` does not change the tempo; that is R3.
- `oLatency` and the target's address are not configurable beyond `--dirt`'s.

## Tests

- `lib/tidal-dirt` (`tests/tidal/dirt.sh`): `tests/tidal/Dirt.hs` builds Tidal's own `/dirt/play` messages for 12 patterns through `toOSC`, over cycles 0 to 4 at 0.5 cps; funk-midi plays the same patterns on its virtual clock and prints the messages it encodes, decoded. All 122 messages match, float bits included. The time tags are not compared: Tidal's come from its Link clock.
- `hosts/dirt` (`tests/hosts/dirt.txt`): time tags, `nudge`, `:cycle`, `:stop` and a pattern of the wrong type.
- `hosts/dirt-handshake` (`tests/hosts/dirt_handshake.py`): a stand-in for SuperDirt that listens only after 2.5 s, replies, then sends `/dirt/hello`. funk-midi must wait, connect, report each once, and shake again.
