# 0.13 acceptance test: plan

Roadmap 0.13 ([roadmap.md](roadmap.md#013----superdirt-sound-in-the-plugin)) has no acceptance test. `lib/dirt-scsynth` checks one UGen, synth or module per case. Nothing checks whole patches, nor the plugin's CPU with `dirt` playing. Planned 2026-11-15.

## Criteria

1. **Sound.** The 22 patches of [patch-port.md](patch-port.md), 18 of which have events in Tidal 1.10.2, rendered through funk's event layer with `d.superDirt`, match SuperDirt 1.7.3's render of the same events in scsynth. Each frame of both channels must be within a stated bound. The bound must also fail the render shifted a frame late.

2. **Real time.** The plugin plays a dense `dirt` set in `test_clap_host --sim` (headless) with no block over budget. `tests/noalloc.c` stays inserted throughout and counts 0 calls.

3. **Cost (decision 14).** For each patch, funk's render CPU time against scsynth's for the same events. A ratio over 2 reopens decision 14 (the threshold is a judgement, not derived).

4. **By ear.** A set on `d1`..`d4` through `dirt` in REAPER on macOS, with `hush`, `xfade` and `setcps`, against the same set through SuperDirt. Needs the user at the DAW.

## Steps

1. **Render in C.** Done 2026-11-15: `funk_player_render_wav` in `graph.h` renders n frames into a 32-bit float WAV file and returns the renders' CPU time. `play.fk` and `oracle.py` use it, so the patches' 6.9M frames (22 patches, 4 cycles each at cps 0.5625, 44.1 kHz) are not printed as text. It saved less than expected: a shard of 31 cases took 86 s in funk, of which the renders took 0.12 s. Most of the rest was `playDirt` building `d.superDirt`'s 74 templates, about 2.4 s per player, in walks that followed every path of the SynthDefs' shared subtrees. With `g.Ref` it takes 0.15 s, and a shard 85 s (CHANGELOG).

2. **Events from Tidal's patterns.** Done 2026-11-15: `tests/dirt/patches.fk` prints each patch's onsets in its first 4 cycles, and `oracle.py`, with `ORACLE_SET=patches`, makes them cases. Planned as: `tests/dirt/patches.py` queries each patch in funk for 4 cycles. `lib/tidal-patches` shows funk's events equal Tidal 1.10.2's (6456 events). It writes the events as `oracle.py` cases: a time and the controls each. The samples come from the events' `s` and `n`, so `d.loadSoundFiles` gets their names, and decision 6's every-folder form is not needed. Samples are read from Dirt-Samples in SuperCollider's downloaded quarks; the test skips (exit 77) without them.

3. **Compare.** Done 2026-11-15: `lib/dirt-patches`, 38 s, so not sharded. 3,095 events in 18 patches match SuperDirt bit for bit: error 0 in every patch, against 0.089 to 0.79 for funk's render a frame late. It gates on the oracle's bound, 1e-6. 56 events of `nn`, a folder Dirt-Samples lacks, are dropped from both, as SuperDirt drops a sound it does not find. A case lists its buffers, and each event picks one as `DirtSoundLibrary.getEvent` does: the folder's files sorted, `n` truncated, `wrapAt`. funk loads each event's `s:n` name, whose file `funk_sample_path` wraps the same way; a host does not (dirt-sound.md, decision 6). `play.fk` sends each event 4096 frames ahead of its frame, as a host sends a window: `p9`'s 730 events overflow the host queue's 256 sent at once. Planned as: `oracle.py` takes the cases with a case set's name. A new ctest, `lib/dirt-patches`, sharded as `lib/dirt-scsynth` is. First, measure each patch's worst frame error. Then propose the bound to the user before it gates the test. Expected (inference): 1e-6 at most, from overlapping events of today's 1.2e-7, unless an FFT module plays (2e-5, difference 13).

4. **CPU against scsynth.** `render.scd` times each case's `Score.recordNRT`; `funk_player_render_wav` returns funk's. Both run offline on the same events, so the ratio compares the engines alone. The test prints the ratios and does not gate on them: machines differ.

5. **Dense set, headless.** `tests/hosts/sim-dirt.fk`: a preamble defines `dirt = d.superDirt` with the patches' samples, and the steps play patches on `d1`..`d4`, then `hush`, `xfade` and `setcps`, with synths (`superfm`, `superprimes`) and FFT modules. Add `hosts/clap/sim-dirt` and `hosts/clap/sim-dirt/noalloc`. The first gates on 0 blocks over budget, the second on 0 counted calls. Record the longest block against the 10.7 ms budget (512 frames at 48 kHz) in this note.

6. **REAPER.** The user plays `sim-dirt.fk` in REAPER on macOS and listens against SuperDirt. Log friction in [patterns.md](patterns.md#friction-log). Then the Linux run (TODO, "0.10 acceptance test") uses the same script.

## Known limits

- Offline renders have no late events. Late arrival (dirt-sound.md difference 3) is covered by step 5 alone.
- Both engines see the same event list, so step 3 checks sound, not scheduling. 0.11's acceptance checked scheduling: OSC time tags, in a capture.
- dirt_monitor's limiter delays both renders by 881 frames; the comparison includes its tail.

## Open, for the user

- The sound bound, once step 3 has measured it.
- Whether criterion 3's threshold of 2 holds, or another.
- Whether step 6 is part of 0.13's acceptance, or follows the release as the Linux run does.
