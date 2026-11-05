# The plugin's simulator mode

Status: implemented, 2026-11-04, for the 0.10 acceptance test ([roadmap.md](roadmap.md#010----stabilization)).

The simulator evaluates steps from a script, one every interval, as a performer would. It logs each step and the plugin's output. It also logs how long `process()` took against the block's duration. Code: `projects/clap/simulator.{hpp,cpp}`. Test: `hosts/clap/sim`.

## Variables

Read once per instance, at `init`. Unset `FUNK_MIDI_CLAP_SIM` turns the simulator off.

| Variable | Value | Default |
|-|-|-|
| `FUNK_MIDI_CLAP_SIM` | the script's absolute path | off |
| `FUNK_MIDI_CLAP_SIM_ORDER` | `rotate` or `random` | `rotate` |
| `FUNK_MIDI_CLAP_SIM_INTERVAL` | seconds from one step's start to the next, >= 0.1 | 16 |
| `FUNK_MIDI_CLAP_SIM_LOG` | the log's path; appended to | the script's path + `.log` |
| `FUNK_MIDI_CLAP_SIM_SEED` | an unsigned integer | the clock's; logged |

- `rotate` starts instance N at step N. `random` never picks a step twice in a row; instance N uses seed + N.
- Every instance reads the same script and appends to the same log. Lines carry `#N`, the instance's number in the process.
- The simulator turns Live on. A project saved with Live off loads in clip mode. The log's step lines then say `clip`.

## Script

`projects/funk-clap/sim-set.fk` is an example.

```haskell
-- the preamble: before the first step, and again when it changes
sig = voices (\gate freq -> gate * d.saw freq * 0.2)

--- hats
pat = p.fast 8 (p.fastcat [p.atom 84, p.silence])

--- error
pat = p.atom nosuch
```

- A `--- name` line starts a step. A `---word` line is a comment.
- Blank lines separate paragraphs. Each is evaluated alone, as Ctrl+Return evaluates it. A paragraph of only comments is skipped.
- The script is read again before each step, so it can be edited during a run.
- A step waits for its evaluations. An expression of type `Pattern Int` waits one cycle, 2 s at 120 BPM.

## Log

```text
2026-11-04T14:50:13.953 #1 step  7: step 7 of 8, live: kernel: fold
2026-11-04T14:50:14.070 #1 out   kernels loaded in 117 ms
2026-11-04T14:50:14.070 #1 done  117.2 ms
2026-11-04T14:50:14.074 #1 audio blocks 20, over budget 0, over half 0, longest 549 us of 10667 us
```

| Kind | Line |
|-|-|
| `start`, `end` | the configuration; the instance's destruction |
| `wait` | until the host's first block |
| `step` | the step chosen, and live or clip |
| `out` | the plugin's output: the code echoed as `sim>`, results, errors |
| `done` | one paragraph's evaluation time; `failed` if its output has `error` or `not playing` |
| `busy` | an evaluation still running after 10 s |
| `audio` | block timings since the last report, after each step |
| `slow` | the same, within 250 ms of any block over half its budget |

A block is over budget when `process()` takes longer than the block lasts: `frames / rate`. Offline blocks are not counted.

## Limits

- The timings are this plugin's `process()` only. The host's deadline also covers other plugins and its own mixing. REAPER's anticipative FX processing runs plugins ahead of the device (REAPER's documentation; not checked here), so an over-budget block may not be audible. Count the device's underruns in REAPER as well.
- The simulator does not press Stop, move the transport or change the tempo. Do those by hand.
- An evaluation that never ends stops the simulator. The log shows `busy`.

## In REAPER

1. `make clap`.
2. In REAPER's preferences, under Plug-ins > CLAP, add `build/clap` to the paths and rescan. `funkdsp.so` must stay beside `funk-midi.clap`. A symlink to the `.clap` alone loses the kernels: the plugin looks beside the path it was loaded from.
3. Start REAPER with the variables set:

   ```sh
   FUNK_MIDI_CLAP_SIM=$PWD/projects/funk-clap/sim-set.fk \
   FUNK_MIDI_CLAP_SIM_INTERVAL=8 FUNK_MIDI_CLAP_SIM_ORDER=random reaper
   ```

4. Insert `funk-midi` on one or more tracks. Steps start at each instance's first block.
5. Read the log: `grep -E ' (slow|busy|error) |failed' projects/funk-clap/sim-set.fk.log`.
