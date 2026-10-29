# DSP graph measurements, 2026-10-29

[audio.md](audio.md), D, item 5. Apple M1 (performance core), Apple clang `-O2`, 48 kHz. `build/dspbench build/funkdsp.so [BLOCK]` (`bench/dsp.c`); the best of 7 runs, each about 20 ms of calls.

## One kernel, ns per 64-frame block

| Kernel | ns |
|-|-|
| `dsp_const` | 4.3 |
| `dsp_add`, `dsp_mul` | 5.3 |
| `dsp_lowpass1` | 205 |
| `dsp_adsr` | 204 |
| `dsp_saw` | 211 |
| `dsp_square` | 266 |
| `dsp_sine` | 287 |
| `dsp_lowpass` (SVF) | 336 |

## The graph's cost per node

256 `dsp_mul` nodes in a chain: 10.5 ns per node per block in the graph, 5.3 called alone. The runner adds about 5 ns per node: the input pointers, the control pass, the loop.

## One voice

`d.poly`'s voice: a saw at a control's frequency, `lowpass 2000 (dc 0.7)`, times `adsr` of a gate control. 12 nodes: 2 controls, 6 constants, 4 kernels. Graphs of 64 voices summed; one voice alone costs the same within 2%.

| Block | funk kernels | same in C | fused C loop |
|-|-|-|-|
| 16 | 329 ns, 1,012 voices | 246 ns, 1,355 | 84 ns, 3,952 |
| 64 | 792 ns, 1,683 voices | 816 ns, 1,634 | 292 ns, 4,564 |
| 512 | 7,391 ns, 1,443 voices | 8,145 ns, 1,310 | 2,570 ns, 4,151 |

Voices: how many fit in the block's duration (1.33 ms at 64 frames) on one core at 100%. A plugin gets a fraction of that.

- **funk's kernels cost what C's do** at 64 and 512 frames. At 16, funk's are 34% slower; the cause is unexamined.

- **The graph costs 2.7x a fused loop.** Only about 60 ns of the 500 ns gap is the runner (12 nodes at 5 ns). The rest is inferred to be latency: each kernel's loop waits on its own frame-to-frame dependency, so the SVF runs at about 16 cycles a frame, and separate kernels run their chains one after another. A fused loop overlaps them: it costs 292 ns, less than saw (211), SVF (336) and ADSR (204) summed, and about the SVF alone. Two other suspects were measured and ruled out in a C copy of the SVF: state through a pointer instead of locals costs 5% (337 against 320 ns); dividing by the rate per frame instead of multiplying costs 3% (204 against 199).

Implications:

- Fusing kernels, as Faust compiles per sample, would gain about 2.7x per voice. So would a kernel that steps several voices in one loop, since the voices' chains are independent; that needs no fusion compiler, but a kernel per voice count.

- At 1,600 voices per core, neither is needed for the plugin.

## Compile time

`tools/kernels.sh` on `lib/kernels.fk`, 11 exports: `emit-c` 10-20 ms, `cc -O2` 140-170 ms. A file with one `dsp_mul` export: 120 ms in all. `cc` on an empty file takes 70-80 ms, so most of a kernel's compile is fixed: process start, the shared library's link, and the 700-line preamble every output carries.

## Friction writing kernels

Rows 8-11 of [patterns.md](patterns.md#friction-log)'s friction log.
