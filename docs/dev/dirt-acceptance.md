# 0.13 acceptance test: plan

Roadmap 0.13 ([roadmap.md](roadmap.md#013----superdirt-sound-in-the-plugin)) has no acceptance test. `lib/dirt-scsynth` checks one UGen, synth or module per case. Nothing checks whole patches, nor the plugin's CPU with `dirt` playing. Planned 2026-11-15.

## Criteria

1. **Sound.** The 22 patches of [patch-port.md](patch-port.md), 18 of which have events in Tidal 1.10.2, rendered through funk's event layer with `d.superDirt`, match SuperDirt 1.7.3's render of the same events in scsynth. Each frame of both channels must be within a stated bound. The bound must also fail the render shifted a frame late.

2. **Real time.** The plugin plays a dense `dirt` set in `test_clap_host --sim-dirt` (headless) with no block over budget. `tests/noalloc.c` stays inserted throughout and counts 0 calls.

3. **Cost (decision 14).** For each patch, funk's render CPU time against scsynth's for the same events. A ratio over 2 reopens decision 14 (the threshold is a judgement, not derived).

4. **By ear.** A set through `dirt` in REAPER on macOS against the same set through SuperDirt. Needs the user at the DAW. Through `pat`, as step 5: `d1`..`d4`, `hush`, `xfade` and `setcps` wait for slots in the plugin (TODO, "R1 in the plugin").

## Steps

1. **Render in C.** Done 2026-11-15: `funk_player_render_wav` in `graph.h` renders n frames into a 32-bit float WAV file and returns the renders' CPU time. `play.fk` and `oracle.py` use it, so the patches' 6.9M frames (22 patches, 4 cycles each at cps 0.5625, 44.1 kHz) are not printed as text. It saved less than expected: a shard of 31 cases took 86 s in funk, of which the renders took 0.12 s. Most of the rest was `playDirt` building `d.superDirt`'s 74 templates, about 2.4 s per player, in walks that followed every path of the SynthDefs' shared subtrees. With `g.Ref` it takes 0.15 s, and a shard 85 s (CHANGELOG).

2. **Events from Tidal's patterns.** Done 2026-11-15: `tests/dirt/patches.fk` prints each patch's onsets in its first 4 cycles, and `oracle.py`, with `ORACLE_SET=patches`, makes them cases. Planned as: `tests/dirt/patches.py` queries each patch in funk for 4 cycles. `lib/tidal-patches` shows funk's events equal Tidal 1.10.2's (6456 events). It writes the events as `oracle.py` cases: a time and the controls each. The samples come from the events' `s` and `n`, so `d.loadSoundFiles` gets their names, and decision 6's every-folder form is not needed. Samples are read from Dirt-Samples in SuperCollider's downloaded quarks; the test skips (exit 77) without them.

3. **Compare.** Done 2026-11-15: `lib/dirt-patches`, 38 s, so not sharded. 3,095 events in 18 patches match SuperDirt bit for bit: error 0 in every patch, against 0.089 to 0.79 for funk's render a frame late. It gates on the oracle's bound, 1e-6, as the user chose. 56 events of `nn`, a folder Dirt-Samples lacks, are dropped from both, as SuperDirt drops a sound it does not find. A case lists its buffers, and each event picks one as `DirtSoundLibrary.getEvent` does: the folder's files sorted, `n` truncated, `wrapAt`. funk loads each event's `s:n` name, whose file `funk_sample_path` wraps the same way; a host does not (dirt-sound.md, decision 6). `play.fk` sends each event 4096 frames ahead of its frame, as a host sends a window: `p9`'s 730 events overflow the host queue's 256 sent at once. Planned as: `oracle.py` takes the cases with a case set's name. A new ctest, `lib/dirt-patches`, sharded as `lib/dirt-scsynth` is. First, measure each patch's worst frame error. Then propose the bound to the user before it gates the test. Expected (inference): 1e-6 at most, from overlapping events of today's 1.2e-7, unless an FFT module plays (2e-5, difference 13).

4. **CPU against scsynth.** Measured 2026-11-15, on an Apple Silicon Mac. `ORACLE_CPU=1` prints each case's render CPU in both engines. funk's figure comes from `funk_player_render_wav`. scsynth's comes from re-running `scsynth -N` on the case's score, minus the same score cut to its time-0 bundles. Each figure is the least of 3 runs. scsynth's startup takes 1.06 s and varies by about 10 ms, so a case needs a render of 10 ms or more. `ORACLE_REPEAT=K` plays a case's events K times to get one. No ctest sets either: the 6 extra scsynth runs per case would double `lib/dirt-patches`, and the plan was to print, not gate.

   | case | funk, s | scsynth, s | ratio |
   |-|-|-|-|
   | 18 patches (samples only) | 0.051 to 0.706 | 0.017 to 0.444 | 1.05 to 3.7 |
   | `plain`, x60 (18 s) | 0.108 | 0.024 | 4.6 |
   | `supersaw`, x60 | 0.195 | 0.074 | 2.6 |
   | `superfm`, x60 | 0.278 | 0.087 | 3.2 |
   | `superpiano`, x60 | 0.171 | 0.049 | 3.5 |
   | `superprimes`, x60 | 1.039 | 0.354 | 2.9 |
   | `superwavemechanics`, x60 | 0.923 | 0.285 | 3.2 |

   Idle, 16 orbits over 7.4 s, costs 10 ms in funk and 5 to 10 ms in scsynth. The ratio is over 2 for samples as for synths, so its cause is not decision 14's graph of UGens. A sample's events play hand-written kernels, and `plain` has the highest ratio. Profiles (`sample`) put the render time in single kernels. In the patches, `dirt_reverb` takes 55%, `dirt_monitor` 15% and `dirt_sample` 13%. In the synths, `GVerb` dominates, ahead of `dirt_reverb`. The graph's own nodes (`BinaryOpUGen`, `Pan2`, `Select`) take 2 to 3%. A fused SynthDef kernel would call the same code, so the fallback in decision 14 would not close the gap. Inference, from `kernels.c`: generated kernels keep state as doubles in an array, read and written through helpers each sample, with int conversions. scsynth's UGens keep state in struct fields and use nova-simd.

5. **Dense set, headless.** Done 2026-11-15: `tests/hosts/sim-dirt.fk`, in `hosts/clap/sim-dirt` and `hosts/clap/sim-dirt/noalloc`. The plugin has no slots (TODO, "R1 in the plugin"), and its host sets the tempo. So the user chose 5 steps of `pat` over `d1`..`d4`, `hush`, `xfade` and `setcps`: samples with global effects, 5 synths, FFT modules, all at once, and silence, on 4 orbits. It uses the test samples, so it runs without Dirt-Samples. The first test gates on 0 blocks over budget, 512 frames at 48 kHz (10.7 ms). It is labelled `realtime`, so CI's shared machines skip it. The second gates on 0 counted calls, in CI.

   Measured on this Mac, 20 s a run. The host's thread first had the default policy. Then the longest block was 9.1 to 11.7 ms, over budget in 4 of 8 runs, with load average 3. Preemption caused most of that. With the time-constraint policy, as Core Audio's IO thread has, the longest was 3.3 to 9.9 ms in 8 runs, none over. The synths step peaks highest. noalloc counted 0 in each run.

6. **REAPER.** Done 2026-11-19, with SuperCollider 3.13.0. `sim-dirt.fk` showed two faults. Neither was in the sound. Its `samples` set peaks at +6 dBFS, buzzes from `crush "4"`, and has no shared grid, in funk and in SuperDirt alike (the oracle matches it within 2.4e-7). The editor's Stop left `dirt`'s events playing (CHANGELOG). So the user listened to `tests/hosts/listen-dirt.fk` instead, against `listen-dirt.tidal`. It has 4 parts, one added per step, on one 8th grid in A minor, and peaks at -7.5 dBFS. The user heard no difference from SuperDirt. Its log had 12 steps, none slow, busy or failed, and 0 blocks over budget, the longest 1.6 ms. The planned run, kept for `sim-dirt.fk`: the user plays `sim-dirt.fk` in REAPER on macOS and listens against SuperDirt. `tests/hosts/sim-dirt.tidal` has the same sets for Tidal 1.10, at cps 0.5, one bar at 120 BPM. In funk and in Tidal, each set has the same onsets over 4 cycles: 144, 72, 88 and 168. The run:

   1. A copy of the script with Dirt-Samples, as SuperDirt plays:

      ```sh
      sed 's|"tests/hosts/samples"|"~/Library/Application Support/SuperCollider/downloaded-quarks/Dirt-Samples"|' \
        tests/hosts/sim-dirt.fk > /tmp/sim-dirt-ear.fk
      ```

   2. REAPER at 120 BPM, as [simulator.md](simulator.md#in-reaper) says, with `FUNK_LIVE_CLAP_SIM=/tmp/sim-dirt-ear.fk` and `FUNK_LIVE_CLAP_SIM_INTERVAL=16`, so each set plays 8 bars.

   3. SuperDirt in SuperCollider, and Tidal, with REAPER stopped; each block of `sim-dirt.tidal` in turn.

   4. `grep -E ' (slow|busy|error) |failed' /tmp/sim-dirt-ear.fk.log`, and REAPER's underrun count. Log friction in [patterns.md](patterns.md#friction-log). Then the Linux run (TODO, "0.10 acceptance test") uses the same script.

## Known limits

- Offline renders have no late events. Late arrival (dirt-sound.md difference 3) is covered by step 5 alone.

- Both engines see the same event list, so step 3 checks sound, not scheduling. 0.11's acceptance checked scheduling: OSC time tags, in a capture.

- dirt_monitor's limiter delays both renders by 881 frames; the comparison includes its tail.

## Open, for the user

- The sound bound: decided 2026-11-15, 1e-6, the oracle's bound. Measured error is 0; exact equality would also fail on rounding a platform may change.

- Criterion 3: decided 2026-11-15. The ratio is 1.05 to 4.6 (step 4), and decision 14 does not cause it. 14 stands, and step 5's budget alone judges cost, as the decision record (dirt-sound.md) words it. Faster generated kernels follow 0.13 (TODO).

- Whether step 6 is part of 0.13's acceptance: decided 2026-11-19, it is (done, above). The Linux run follows the release.
