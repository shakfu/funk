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

## Open

- **The Mac.** On arm64, `floor` is one instruction (`frintm`), and Apple clang's default is `-fno-math-errno` (inference, not checked). If both hold, cost 1 did not apply to step 4's Mac figures, and the stages are the change that counts there. Measure with `ORACLE_CPU=1 ORACLE_REPEAT=60`, and run `lib/dirt-patches` and `lib/dirt-scsynth`. They skip on this machine: there is no SuperDirt quark and no Dirt-Samples.
- **Other kernels with lines in slots.** `dirt_leslie`, `dirt_monitor`, the delays and `GVerb` (`gvDiff`) keep lines in the slot array too. Their per-sample cost is not measured.
- **A compiler fix for cost 2.** The compiler could hoist slot reads out of a self-recursive loop. That is harder, since nothing tells it that `mem` and `st` do not overlap.
- **`-fno-math-errno` for `funkdsp`.** It would remove the remaining `lrint` calls in other kernels. Not adopted: their kernels' share of render time is not measured.
