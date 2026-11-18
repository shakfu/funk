# dirt_reverb's kernel cost, 2026-11-18

TODO, Speed: generated DSP kernels render 1.05 to 4.6x scsynth's CPU ([dirt-acceptance.md](dirt-acceptance.md), step 4), and `dirt_reverb` takes 55% of the patches' render. This note measures that kernel alone and splits its cost.

## Method

AMD Ryzen 9 PRO 6950H, Linux, GCC 13.3, `-O2` as `funkdsp` builds. 64-frame blocks at 48 kHz. A scratch C harness calls `dsp_dirt_reverb` from `funkdsp.so`. It also runs a C port of the kernel's steady-state path, with the same arithmetic and the same slot layout at block boundaries. Within a block, the port keeps every delay line's fields, the ramps and the filters' state in locals. Block 0 runs funk's constructor branch. From then on, both run from copies of its state and memory, with the same inputs: dry noise at 0.3, constant controls, and `resumed` pulsed every 997 blocks.

The port matches funk exactly: output and all 84 state slots differ by 0 over 20,000 blocks. DetectSilence never fires, since the input is never silent.

The CPU's clock moves absolute times by up to 1.5x between runs. So the table gives funk's time over the port's, timed in the same process, each the best of 7 runs of 2,000 blocks.

## Result

| funk kernel | funk / port |
|-|-|
| as built (`-O2`) | 4.2 |
| `-fno-math-errno` | 3.7 |
| `-fno-math-errno -march=x86-64-v2` | 2.24 |
| `cInt` as `truncateSat` (`-O2`) | 1.78 |
| and one stage at a time over the block (`-O2`) | 0.75 |
| port re-reading each line's slots every sample, flags as row 3 | 0.80 |

Two costs:

1. **`cInt` called libm.** It truncated as `lrint (floor x)`, or its mirror for x < 0. Under GCC's default `-fmath-errno`, `lrint` is a PLT call. So is `floor` on x86-64 without SSE4.1. `allpassN` reads 4 slots through `cInt` and runs 6 times a sample, so a sample made about 30 calls. Each call also forced the loop's state out of registers. After the 32-bit clamps, both forms give the same integer, so `cInt` now ends in `truncateSat`, one `cvttsd2si`. The two forms agree on 200M random doubles and on ±0, ±inf, NaN, denormals and the clamp bounds. 4.2 to 1.78.

2. **Delay lines lived in the slot array.** `allpassN` and `delayN` read `off`, `len`, `ds` and `fb` from `st` as doubles every sample, converted them, and wrote `pos` back. The last row shows this costs all of the port's lead. It accounted for the remaining 1.78x.

## Stages

The reverb now runs one stage at a time over the block, as scsynth runs one UGen at a time. LocalIn reads the block before's frames, so within a block the chain has no feedback, and stages can run in sequence. Each delay line is one loop whose fields are parameters, read from the slots once a block. Each stage's output is already rounded to a float, so `out` holds the input allpasses' sum and the LocalIn region of `mem` holds the loop's, with no rounding change.

That gives 0.75x the port, so the kernel is about 5.6x faster than it was: about 1,080 ns a 64-frame block. Output and state stay bit-exact over 20,000 blocks of 17, 64, 256 and 512 frames.

The alternative was one loop with every line's fields as parameters: 7 lines of 4 or 5 fields, about 50 parameters in all. It also needs each line's new position back from `allpassN`, but an exported kernel cannot return a tuple (friction log #8). The staged form needs neither.

`check.sh` rejects `plusPtr` in kernels: an address computed through `uintptr_t` loses provenance, and clang then stops vectorizing. So a stage takes a buffer and a frame offset into it.

## Other kernels

`dspbench`, before and after: `dsp_adsr` 103 to 96 ns, `dsp_adsr_exp` 149 to 137. The others are within 2%, except `dsp_add` and `dsp_mul`, which went from 16 to 30 ns. Their instructions are byte-identical in both builds. Only the address moved: the new loop crosses a 64-byte boundary. That is code placement, not `cInt`.

The `.so` still has 113 `lrint` or `floor` PLT calls, from kernels that call `clrint` and `cfloor` directly. None remain in `dsp_dirt_reverb`.

## End to end, on Linux

The scsynth oracles ran on this machine with SuperDirt v1.7.3, from a scratch clone and its own `sclang -l` config. The installed quark is a develop checkout (`v1.7.4-18-ga3046d1`), whose `.quark` file still says 1.7.3. `lib/synthdefs` fails against it: 15 synths' `speedFreq` default to 1, not 0. Against v1.7.3 it passes.

**Exactness.** funk renders every case and patch byte for byte the same with HEAD's kernels and with these: 141 renders, 123 `lib/dirt-scsynth` cases and 18 patches.

**CPU** (`ORACLE_CPU=1`; the synth cases with `ORACLE_REPEAT=60`). funk's render CPU, before and after:

| | funk before, s | after, s | speed-up | ratio to scsynth, before | after |
|-|-|-|-|-|-|
| 18 patches, summed | 3.250 | 2.009 | 1.62 | 2.5 to 7.9 | 2.1 to 4.0 |
| `plain`, x60 | 0.180 | 0.085 | 2.12 | 8.2 | 3.7 |
| `supersaw`, x60 | 0.290 | 0.147 | 1.97 | 8.5 | 4.1 |
| `superfm`, x60 | 0.461 | 0.278 | 1.66 | 14.0 | 8.4 |
| `superpiano`, x60 | 0.283 | 0.131 | 2.16 | 8.3 | 4.0 |
| `superprimes`, x60 | 1.931 | 1.108 | 1.74 | 5.0 | 2.8 |
| `superwavemechanics`, x60 | 1.646 | 0.785 | 2.10 | 4.9 | 2.3 |

The ratios to scsynth are too high: scsynth here never ran `dirt_delay`, which funk renders (see Open). funk's before and after times compare like with like.

**Why the reverb runs in patches without `room`.** SuperDirt's parent event sets `~dry = 0.0` (`DirtOrbit.sc:187`). So `GlobalDirtEffect.set` finds `dry` non-nil in every event and resumes `dirt_reverb`, and funk does the same (`lib/dsp.fk:502`). The reverb therefore runs for a patch's whole length on its orbit, in both engines, and adds nothing to the sound: replacing it with a pass-through leaves all 18 patches byte-identical. It costs 14% of funk's patch renders now.

**Profile** (callgrind, instructions inside `funk_player_render_wav`, patch `p0`):

| | share |
|-|-|
| `dirt_reverb` (its kernel and `allpassLine`) | 32% |
| `funk_events_read` | 18% |
| `effects` (events.c), and its `memcpy` | 15% |
| `dirt_monitor` | 13% |
| `dirt_switch_delay` | 8% |
| `pow`, `dirt_rand`, `dirt_shape`, `dsp_const`, `dirt_sample` | 1 to 3% each |

Instructions, not time: the reverb's 32% of instructions took 14% of time. `funk_events_read` is not a kernel. It mixes 16 orbits into the output and tests each orbit's `used` flag once a frame, though the flag is set per 64-frame block.

`funk_events_read` now tests `used` once a block, and copies or adds the block's frames in one loop. On `p0` it fell from 120.6M to 12.3M instructions, and the render from 687M to 579M. All 141 renders stay byte-identical. Time not measured.

`effects` (events.c) scanned every template, about 75, for the few effects, once per orbit per block: 81,248 calls in `p0`. It also copied and cleared 2 KB of buffers for idle orbits before returning. It now scans an index of the effects, and returns first for an orbit with no running effect and nothing played. Its own and its `memcpy`'s instructions fell from 101M to 23M, and the render from 579M to 501M. All 141 renders stay byte-identical.

`dirt_monitor` kept its Limiter's state in slots 15 to 25, as the reverb kept its lines. It now runs in three stages: the checked sum, the Limiter, and the select with the envelope. The Limiter runs one loop per segment up to its buffer's end, 441 or 480 frames, with its fields as parameters, and swaps buffers between segments. Its instructions fell from 87.2M to 56.7M, and the render from 501M to 470M. All 141 renders stay byte-identical. They cover limitertype 1, in every case, and 2, in `softclip`; not 0, a pass-through. Per-sample slot traffic remained in `silent`, `trig1` and `rampAt`, which the reverb, the delays, `dirt_leslie` and the monitor share.

`trig1` became `trig1Block`. Each frame stores the trigger as the last one, and the trigger is constant over the call. So `Trig1` can fire only at frame 0, and its output is 1 for a count of leading frames computable once a call. `silent`'s counter became a loop parameter, and its frame limit is computed once a call. The render fell from 470M to 454M instructions. `rampAt` is unchanged: its 4 lanes and step would add 5 parameters per ramp, 45 in `dirt_leslie`.

The 141 renders stay byte-identical. They may not reach a resume during a running `Trig1`, or `DetectSilence` firing mid-block. So a scratch harness ran each of the 5 kernels from both builds on the same inputs, and compared output, every slot, the delay memory and `done` bit for bit. Inputs: noise with silent stretches of up to 20 s, blocks at 5e-5 and at 3.0, `resumed` pulsed at random, in consecutive calls too, and calls of 64 frames or of 1 to 256. The 3.0 blocks drive the Limiter's gain down, and the monitor cycles limitertype 0 to 2. Over 200,000 to 400,000 calls per kernel and mode, all were equal, with `DetectSilence` firing in 47,000 to 320,000 of them. The monitor also matched HEAD's kernels, before its Limiter was staged.

**Time.** The harness also times each kernel's calls, 100,000 calls of 64 frames, best of 3. The CPU's clock moves times between processes, so each pair of builds ran in one process. ns per call:

| kernel | Limiter staged: before | after | `trig1`, `silent`: before | after |
|-|-|-|-|-|
| `dirt_monitor` | 514 | 377 | 374 | 328 |
| `dirt_switch_delay` | | | 513 | 362 |
| `dirt_reverb` | | | 1,120 | 1,054 |
| `dirt_comb_delay` | | | 704 | 700 |
| `dirt_leslie` | | | 3,132 | 3,099 |

**The switch delay's crossfade, tried and reverted.** Slots 21 to 25 as loop parameters made `dirt_switch_delay` 36% slower, 362 to 491 ns a call, and its instructions rose 7%. The loop computed the crossfade's next state every frame. Before, it tested slot 25 and skipped that work; the crossfade runs only for 2,000 frames after a delay change. A form that keeps the gain would run a loop without the crossfade while slot 25 is 0. Not tried.

What remains in these kernels is their own state in slots: the switch delay's crossfade, `dirt_leslie`'s filters and oscillators, and comb's LeakDC. Instruction counts misled once here, so each change needs timing too.

## Open

- **The oracles fail on this machine, at HEAD too: its sc3-plugins has no server plugins.** With v1.7.3, 112 of 123 `lib/dirt-scsynth` cases and all 18 patches fail. `~/.local/share/SuperCollider/Extensions/SC3plugins` holds classes and help but no `.so`. So sclang sees `SwitchDelay`, and SuperDirt builds its `SwitchDelay` `dirt_delay`. scsynth then refuses it: "UGen 'SwitchDelay' not installed", "/s_new SynthDef not found". funk renders that delay as on the Mac, whose sc3-plugins is complete. `SwitchDelay.ar(input, 1, 1, ...)` passes its input at level 1. At each resume, it adds the dry signal to the effect bus, times its `Env.asr` (0.4957 after the first block) and `delayAmp.lag(0.01)` (from 1). That is the 1.4957 seen at `plain`'s onset, fading over about 0.01 s. The 11 that passed delay their sound past that fade: the FFT modules, the comb delays and `pshift`.

  The check: hide the sc3-plugins classes from sclang (`excludePaths`), and render every case with the CombL delay (the oracle's `core` mode). Then 79 of the 82 cases scsynth can render pass. `shape` misses the 1e-6 bound at 2.4e-6, `imp` at 7e-4 and `impAccelerate` at 2.4e-3. Likely floating-point differences between the platforms; not checked.

  Fixes outside the repo: install sc3-plugins' server plugins, or remove its classes. A fix in the repo would be a check in `render.scd` that SuperDirt's UGens load in scsynth, before it renders. The oracles now pass on a machine where they cannot run.
- **The Mac.** On arm64, `floor` is one instruction (`frintm`), and Apple clang's default is `-fno-math-errno` (inference, not checked). If both hold, cost 1 did not apply to step 4's Mac figures, and the stages are the change that counts there. Measure with `ORACLE_CPU=1 ORACLE_REPEAT=60`.
- **Other kernels with lines in slots.** `dirt_leslie`, `dirt_monitor`, the delays and `GVerb` (`gvDiff`) keep lines in the slot array too. `dirt_monitor` takes 13% of `p0`'s instructions, `dirt_switch_delay` 8%.
- **A compiler fix for cost 2.** The compiler could hoist slot reads out of a self-recursive loop. That is harder, since nothing tells it that `mem` and `st` do not overlap.
- **`-fno-math-errno` for `funkdsp`.** It would remove the remaining `lrint` calls in other kernels. Not adopted: their kernels' share of render time is not measured.
