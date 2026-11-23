# funk review at 0.13.0

Date: 2026-11-22. Commit 189e247. Scope: architecture, engineering practice, comparison with current techniques, and use to the target users (Tidal and Strudel live coders, DAW users).

Method:

- Code reading of `projects/funk/src`, `projects/funkgraph`, `projects/clap`, `projects/funk-clap`, `projects/funk-midi` and `lib/`.
- Probe programs run against `build/funk`.
- `make test` run once on this machine.
- Tags: **[V]** verified in code or by a run in this review. **[I]** inference, not executed or measured. Comparisons with other systems come from published work, cited inline, and were not re-checked against those systems here.

Previous review: [docs/dev/261027-lang-review.md](docs/dev/261027-lang-review.md) (0.6.0). This one does not repeat its points unless their status changed.

## 1. Summary

funk does the following well:

- It has a coherent niche. Strict Haskell-syntax code, compiled C, and a collector with pauses of at most 0.5 ms. It runs Tidal patterns inside a CLAP plugin without GHC or SuperCollider.
- Its verification discipline is unusually strong for a one-person language:
  - 571 ctest tests;
  - a 4-OS/arch-by-4-sanitizer CI matrix;
  - differential checks of interpreter against compiled code;
  - oracles against Tidal 1.10.2 and scsynth 3.13.0.

The main risks:

1. **Soundness and memory safety at the edges** (section 3):
   - let-polymorphism of mutable values;
   - a stack overflow in the parser;
   - session teardown races;
   - signal handlers shared across plugin images.
2. **The compiler's shape** (section 2.1). The middle end consists of one mutable AST and pattern-specific optimizations, with no core IR or simplifier. This drives the performance drops the 0.6 review named, and it makes each pass hard to test alone.
3. **Access for the target users** (section 5):
   - building from source with a C toolchain;
   - `funk-midi` that is not relocatable;
   - single-line input in `funk-midi`;
   - no editor integration beyond tree-sitter;
   - user docs of about 840 lines against 6908 lines of dev notes.

Test status [V]: 570 of 571 passed. `hosts/clap/sim-dirt` failed twice while three review agents loaded the CPU ("2 in 13 reports" over budget, longest block 15120 us of 10667 us). It passed on an idle machine. The test measures wall-clock budget, so it depends on machine load. Its pass is real only on a quiet machine. Under host load in a DAW, the same set would miss blocks [I].

## 2. Architecture

### 2.1 Compiler

The pipeline is lex, parse, qualify, check, elab (dictionary passing, lambda lifting, fusion, higher-order specialization, cell reuse), resolve, then interp or cgen. The 13 compiler files total 13,099 lines; check.c is 3138, elab.c 2253, parse.c 1926 and cgen.c 1507.

Strengths:

- **Global state is per session.** The 0.6 review counted about 165 file-scope globals. They are now 109 fields in per-session structs (`CheckState` 26, `CompilerShared` 28, `ElabState` 16 and others) [V]. They are reached through `fk_here()`, which finds the session from the stack pointer (`rt.h:110`). The state is still implicit, but it is no longer process-wide.
- **The HM core has the standard sound structure** [V]: level-based generalization, an occurs check with level adjustment, and skolem escape checks (`check.c:342-437`). Bidirectional checking places errors at the innermost wrong sub-expression.
- **Chunks roll back on error** through an undo log and table truncation. This is the right model for a REPL and a plugin.
- **Fusion now recognizes prelude functions by binding identity, not by name** (`fuse.c:1190`) [V]. A user binding that shadows a prelude name no longer triggers it.

Weaknesses:

- **One AST carries every phase.** `Expr` holds `ty`, `vk`/`idx`, `lazy`, `own`, `preds`, `known` and `global_ref` (`funk.h:142-170`). `Bind` holds `orig`, `linear`, `own_key`, `own_of`, `from`, `pure` and `total` (`funk.h:190-216`). Passes mutate it in place [V]. No pass has a typed input or output contract. The only invariant check is `FK_CHECK_TYPES`.
- **check.c drives the pipeline.** `compile_chunk` sequences elab, lift, fuse, lift, ho_specialize, reuse, dicts and resolve (`check.c:2989-3032`). The checker also calls into the backend: `cgen_alloc_free` at `check.c:2878` [V].
- **elab.c holds four passes:** dictionary passing, lifting, higher-order specialization and reuse.
- **There is no general simplifier.** There is no inliner, no case-of-case and no case-of-known-constructor [V, by grep]. Each optimization recognizes a source shape instead:
  - fusion covers 10 prelude functions (`fuse.c:588`), with `foldl` the only consumer; `zipWith`, `zip`, `foldr`, `scanl` and `iterate` are not covered;
  - specialization keys on a string of the argument's structure. It stops at 160 characters per argument, 512 per key, 4 free locals and 4000 copies (`elab.c:1059-1102`);
  - reuse needs a static linearity proof, for at most 16 parameters and 30 fields;
  - TRMC handles self-recursion with the cons in the last argument only.
- **Two backends implement the semantics.** The 0.6 review's open question, whether to keep the interpreter, is still open in [roadmap.md](docs/dev/roadmap.md#open-questions). The interpreter is now load-bearing: patterns run interpreted in `funk-midi` and the plugin, and `examples/` are checked in both modes. The question has become "how to share one core", not "whether to drop the interpreter".
- **Hard limits** [V]:
  - 64 classes, as a `uint64_t` mask (`check.c:205`, `check.c:2350`); the libs declare 18;
  - tuples of at most 7;
  - classes cannot be redefined in a session (`check.c:2349`);
  - `Type` has no application of a type variable (`funk.h:365`), so there are no higher-kinded classes, and `fmap` is monomorphic on `Pattern` (`lib/pattern.fk:345`).

### 2.2 Runtime

Values are one tagged word. The collector is a conservative, non-moving mark-sweep:

- size-class chunks in one reserved range;
- incremental snapshot-at-beginning marking;
- a deletion barrier the compiler emits at three stores;
- copy-on-write protection of stack pages;
- lazy sweeping.

Findings [V unless tagged]:

- **The write-barrier claim holds.** The only heap-pointer overwrites are:
  - the thunk update (`rt.c:1346`);
  - the in-place reuse store (`cgen.c:406`);
  - the TRMC destination (`cgen.c:978`).

  Other stores go into objects allocated black, or into root tables that the start pause scans in full. `Array` and `Ring` are C memory that the collector does not scan. A heap value enters it only as a `StablePtr`.
- **Audio rendering never touches the collector.** `funk_player_render_channels` (`graph.c:469`) runs compiled C kernels only. Graph swaps use an atomic exchange with epoch-deferred reclaim. The SPSC rings and the Vyukov MPSC ring use correct acquire and release ordering.
- **POSIX only.** The runtime depends on:
  - `mmap(MAP_NORESERVE)`;
  - `sigaction` with ucontext PC and SP rewriting;
  - `sigaltstack`, `pthread_attr_setstack`, `dlopen` and `posix_spawnp`;
  - a 4 GiB stack in an 8 GiB-aligned slot.

  A Windows port means a new stack and fault-handling layer (vectored exception handling, `VirtualProtect`), not a shim. CLAP users on Windows are excluded until then [I].
- **Pacing is fixed:** 0.4 ms steps every 256 KiB, a goal of 2x live, and a forced finish when allocation outruns marking (`rt.c:1043`). The forced finish is an unbounded pause with no mutator assist. Go's pacer handles this case with assists and feedback ([Go GC pacer redesign](https://github.com/golang/proposal/blob/master/design/44167-gc-pacer-redesign.md)).

### 2.3 Hosts

`funk-midi` is a REPL player for MIDI, SuperDirt OSC and internal audio. `funk-clap` and `projects/clap` form the CLAP plugin. `funkgraph` is the audio graph runner.

- A failed compile keeps the old kernels and graph playing (`funk_clap.cpp:647-655`) [V]. A failed definition keeps the old `pat` [V, per music.md and the agent's run]. A `pat` that fails while it plays goes silent until it is redefined. That is less forgiving than the compile-error policy [I].
- The interpreter's FFI and user `foreign export` kernels run `$CC` at run time, inside the DAW. The plugin avoids this for `graph.fk` through linked glue. On macOS without the Command Line Tools, `/usr/bin/cc` prompts for an install [I].

## 3. Correctness and safety findings

Ranked. Each finding names its evidence.

| # | Severity | Finding | Evidence | Action |
|-|-|-|-|-|
| C1 | high | **No value restriction.** A `let` of a mutable value generalizes, so one `Array` is written at `Float` and read at `Int`. Through the public api: `let a = newArray 2 nullPtr` stores a `Ptr Int` and reads it as `Ptr Float` with no `castPtr`. With pointer element types this corrupts memory. | [V] Repro below; `check.c:615-623`, `check.c:2020` | Do not generalize a binding whose evaluation acts. The effect system already knows this (`acts_by`). This is Garrigue's relaxed value restriction ([Garrigue 2004](https://caml.inria.fr/pub/papers/garrigue-value_restriction-fiwflp04.pdf)). Add both repros to `tests/errors`. |
| C2 | high | **Stack buffer overflow in `tuple_name`.** `char buf[16]` with `strcat` (`parse.c:276-280`). The tuple-section path calls it without the `FK_MAXTUPLE` check (`parse.c:634`); so does an instance head (`check.c:2442`). | [V] `main = print (length [(,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,)])` exits 133 (SIGTRAP). In a plugin this kills the DAW. | Check the bound inside `tuple_name`; remove the fixed buffer. |
| C3 | high | **Session teardown frees memory that other threads may still use.** `fk_stop` stores NULL to the registry slot, then frees `r`, `r->ring` and the semaphore (`rt.c:2649-2681`). A MIDI callback thread inside `fk_event_claim`/`fk_event_publish`, or `on_fault` on another thread, can still hold `r`. | [V] code; [I] no repro | Add an epoch or reference count around `session_of` users. At minimum, document that the host must stop foreign callback threads before `funk_free`. |
| C4 | high | **Signal-handler restore breaks with a second libfunk image.** `handlers_off` restores `prev_segv` unconditionally (`rt.c:2558-2562`). With two plugins, each with its own libfunk, or a crash reporter installed after funk, the first image's restore removes the second's handler. The second image's next copy-on-write fault then kills the process. If the first image is unloaded, a saved `prev_segv` points into unmapped code. | [V] code; `ffi.c:288` anticipates two images | Restore only if the current handler is still this image's. Simpler: install once per process and never uninstall. |
| C5 | medium | **CLAP `reset` does blocking work on the audio thread.** The spec puts `reset` on the audio thread. `plugin.cpp:364` calls `stop()`, which takes `savedMutex`, frees a `Clip`, swaps a deque under a mutex, fulfils promises and notifies a condition variable (`plugin.cpp:443-452`). | [V] | Set atomics in `reset`; do the rest on the evaluator thread or through `request_callback`. |
| C6 | medium | **Saved project state can re-enable the unauthenticated UDP evaluator** (`kStateUdp`, `plugin.cpp:687`, `plugin.cpp:731`). It binds 127.0.0.1, but any local process can then make the DAW compile and load C. | [V] | Do not restore UDP from state without user action, or require a token. |
| C7 | medium | **`spawn_wait_err` ignores `waitpid`'s result** (`util.c:322`). On `EINTR` it reads an uninitialized `status`. It may then rename a `.so` the compiler is still writing. Under a stack snapshot, `waitpid` writing `&status` into a read-only page fails with `EFAULT` instead of faulting. `stat` at `ffi.c:259` and `ffi.c:280` has the same problem. | [V] | Loop on `EINTR` and check the return value. Release the snapshot or keep buffers off the stack at these calls. |
| C8 | medium | **Chunk rollback is incomplete.** `CS->ffi_exports`, `CS->all_lambdas` and the intern table are not truncated (`check.c:2903-2925`). If export 2 fails, export 1 stays, with a freed `gidx` that `cgen_kernels` reaches later (`cgen.c:1434`). | [V] code; [I] affects only hosts that compile again after a failure: the plugin and `funk-midi` | Truncate them in the `setjmp` branch. Push exports after the loop. |
| C9 | medium | **The specialization budget is per session.** `ST->nho` counts to 4000 and is never reset, not even by `elab_abort` (`elab.c:1059`, `elab.c:1158`, `elab.c:1294`). In a long live set, specialization stops silently for every later input. | [V] code; [I] how fast a set reaches 4000 is unmeasured | Reset it per chunk. Emit an opt-report note when the cap is reached. |
| C10 | medium | **Compiler memory is never freed during a session** (`util.c:12-38`, "what was allocated stays until the session ends"). `charge` counts allocations, not live bytes, and bills a full `xrealloc` instead of the delta (`util.c:67`). Memory grows with every evaluation. | [V] code; [I] growth rate over a multi-hour set is unmeasured | Use one arena per chunk, and keep only what installed globals reference. Measure memory after 1000 evaluations as a test. |
| C11 | medium | **Primitive totality is encoded twice, with opposite polarity.** `fuse.c:60-67` is a deny-list by funk name, so it fails open. `resolve.c:63-71` is an allow-list by C name, so it fails closed. | [V]; no current miss found | Add a totality and effect column to `prims.def`, and derive both checks from it. |
| C12 | medium | **Stable pointers carry no generation.** They hold a session id and slot only (`rt.c:1992`). A later session that reuses the id receives a stale pointer's events. A reused slot can alias within one session. | [V] | Put a generation in the spare bits. |
| C13 | medium | **A race on stack pages written from another thread.** `on_fault` can copy a page on a foreign thread while `scan_snapshot` reads `snap_copied` as plain memory (`rt.c:713`, `rt.c:2484`). | [V] code; [I] needs C code that writes funk's stack from another thread | Abort on a foreign-thread fault, or use a per-page atomic state. |
| C14 | low | The FFI cache falls back to `/tmp/.cache/funk/ffi` with no owner or mode check, then `dlopen`s any `<hash>.so` there (`ffi.c:236`, `ffi.c:435`). The 64-bit FNV-1a key is not collision-resistant. Kernel keys omit headers and libraries (`ffi.c:490`). | [V] | Require a user-owned 0700 directory. Add the compiler version to the key. |
| C15 | low | `abort()` on internal invariants (`elab.c:218`, `cgen.c:873`, `check.c:261` and others) and `exit(1)` from `cerror` without a jump target (`util.c:290`) kill an embedding host. | [V] | Route to the session's error jump in library builds. |
| C16 | low | `funk_on_fatal` ignores its session and writes a process global (`driver.c:781`). | [V] | Store it per session. |

C1 repro [V], in both backends:

```haskell
main = do
  let arr = primNewArray 4 8
  writeArray arr 1 (2.5 :: Float)
  print (readArray arr 1 :: Int)   -- 4612811918334230528
```

## 4. Engineering practice

Strengths [V]:

- **CI.** It runs on ubuntu-latest, ubuntu-24.04-arm and macos-latest, each plain and under UBSan, ASan and TSan, plus a plugin job on Linux and macOS (`.github/workflows/ci.yml`).
- **Differential testing.** Each run program is checked against one `.expected` in interpreter, compiled, GC-stress and TSan modes.
- **External oracles.** Tidal 1.10.2 events and scsynth 3.13.0 renders are the reference.
- **Allocation checks.** A no-allocation checker (`tests/noalloc.c`) covers the audio-thread paths.
- **Golden snapshots** of fused loops, reuse copies and the opt-report.
- **Performance discipline.** Interleaved A/B timing (`bench.py ab`), and a recorded history.
- **Decision records** in `docs/dev/` name the alternatives they rejected.

Gaps:

- **No pass-level tests.** Passes read session state, so one cannot be run alone. There is no AST dump between passes. Only fusion has an off switch (`FK_NOFUSE`). Add `FK_DUMP=<pass>`, per-pass off switches and an invariant check after each pass.
- **The benchmark record is 22 days old.** The last `bench/results.jsonl` entry is 2026-10-31. The 1.18x C figure in TODO.md predates 0.11 to 0.13 [V]. Those releases mostly added library code, but they also changed generalization and literal typing. Re-run `make bench` before quoting the figure.
- **Real-time tests depend on wall-clock time.** `sim-dirt` fails under CPU load (section 1). Either label these tests `exclusive` (ctest `RUN_SERIAL`) and keep the wall-clock check, or check a CPU-time budget per block.
- **Optional oracles pass silently.** Without sclang the scsynth oracles are not registered, so `make test` passes without them (TODO.md, High). Fail when `SCLANG` is set but does not run.
- **Generated code has no generator.** `lib/control.fk` (4213 lines, 980 signatures) says it is "generated from Params.hs" (`control.fk:1277`). No generator is in `scripts/` or `tests/` [V]. It cannot be regenerated for a new Tidal release. `lib/synthdefs.fk` does have its generator (`tests/dirt/synthdefs.scd`).
- **Stale documentation** [V]:
  - README.md:184-194, "Not supported", lists "modules and imports". Modules shipped in 0.6.0, and the README documents them 90 lines earlier.
  - The `docs/dev/architecture.md` line table dates from 0.1: cgen 321 (actual 1507), elab 475 (2253), check 1966 (3138), rt 898 (2690). Its line 236 says "one session per process". 0.8 removed that limit.
  - `examples/tidal/README.md:31` says the hosts play 0.5 cps and that Tidal's default is 0.5625. `docs/music.md:45` and `--help` say 0.575.
  - The summary table in `docs/dev/tidal-comparison.md:31` lists 8 SuperDirt-sound gaps. Its body marks all but S8 done.
  - `docs/music.md`'s `funk-midi` option table omits `--audio`, `--in`, `--in-virtual`, `--kernels` and `--voices` [agent run, not re-checked].
- **Install covers only `funk`.** `make install` installs `funk` alone (`Makefile:62`). `funk-midi` compiles in `FUNK_LIB_DIR="${funk_SOURCE_DIR}/lib"` (`projects/funk-midi/CMakeLists.txt:16`), so it needs the source tree at run time [V].

## 5. Use to the target users

Target, per the 0.6 addendum: Tidal and Strudel live coders, with DAW plugins and embedded hosts.

What funk offers that alternatives lack [V unless tagged]:

- **Tidal semantics without GHC or SuperCollider.**
  - Core.hs, UI.hs and Control.hs are covered, with 178 cases in `lib/tidal`.
  - All 257 controls take Tidal's types.
  - Slots `d1`..`d16`, transitions and `setcps` work in `funk-midi`.
  - All 6456 events of the 22 `cycles.tidal` patches match Tidal 1.10.2.
- **Tidal inside a DAW** as a CLAP plugin, with host tempo, paragraph evaluation and kernels compiled while playing.
- **Internal SuperDirt sound.** 37 synths and 19 effects are checked against scsynth renders, for hosts without SuperCollider.
- **Pattern evaluation is fast enough to play.** One 6-combinator `d1` line took about 25 ms interpreted [agent run]. The slowest 10 ms query window took 2.0 ms (patch-port.md).

What blocks adoption, in order of cost to the user:

1. **Installation.** The user must build from source with CMake 3.20+, a C11 compiler and `-DFUNK_LIBREMIDI=ON`, then fetch about 400 MB of samples. There is no binary, no package, no Windows support, and no relocatable `funk-midi`. Strudel needs a browser tab ([strudel.cc](https://strudel.cc)). Tidal needs GHC and SuperCollider, but ships installers and documented setup ([tidalcycles.org](https://tidalcycles.org)).
2. **Multi-line input in `funk-midi`.** Input is one `getline` per evaluation (`projects/funk-midi/main.c:1003`) [V]. A `d1 $ s "bd*2"` followed by an indented `# gain 0.8` fails on the second line [agent run]. Multi-line `d1` blocks are the standard Tidal idiom. Plain `funk` has `:{ :}`; `funk-midi` does not.
3. **Editor integration.** There is a tree-sitter grammar for Helix and Neovim, and no LSP. There are no packages for VS Code, Emacs, Vim or Pulsar, which Tidal users evaluate blocks from. The plugin's own editor is the only block-evaluation workflow. TODO.md already lists `funk-midi --watch FILE`.
4. **Language gaps Tidal user code hits** [agent runs]:
   - no `type` synonyms;
   - no as-patterns;
   - no list comprehensions;
   - no infinite ranges;
   - `f.g` is field access;
   - `String` is not `[Char]`;
   - no higher-kinded `Functor`/`Applicative` (`<$>` works on `Pattern` only);
   - no `Rational` (T3).

   Tidal one-liners rarely hit these. Helper functions in shared sets often do [I].
5. **User documentation.** User-facing text is about 840 lines: README, `docs/music.md` and `examples/tidal/README.md`. There is no function reference and no "coming from Tidal" guide. `docs/music.md` teaches `p.fastcat [p.atom 60 ...]` before `d1 $ n "0 4 7" # s "superpiano"`.
6. **The namespace of `pattern`.** It has no export list, so all 673 of its names are unqualified in the hosts. This has already forced two renames (`clip` to `clipEvent`, `sect` to `sects`).

Assessment [I]: compiled speed and the real-time collector are invisible to a pattern-only user. They matter for the plugin and for users who write DSP. funk's distinct value for this audience is "Tidal in the DAW, no SuperCollider". The remaining gap there is R1: slots `d1`..`d16` do not work in the plugin (TODO.md).

## 6. Comparison with current techniques

| Area | funk | Current practice | Assessment |
|-|-|-|-|
| Effects | `->>` arrow, inferred, one effect per signature | Koka row-polymorphic effects ([Leijen 2014](https://arxiv.org/abs/1406.2061)); OCaml 5 effect handlers | It is light and fits strict code. It is less expressive (no handlers, one effect per signature), and that is adequate for the niche. It should also drive the value restriction (C1). |
| Classes | single-parameter, 64-bit mask, no kinds | GHC kinds and multi-parameter classes; Lean 4 instances | The mask and the missing type application block `Functor`/`Applicative`, which Tidal code uses. |
| Middle end | surface AST mutated in place; shape-specific passes | typed core IR plus simplifier (GHC Core, Lean's LCNF, OCaml flambda) | This is the main structural debt (2.1). |
| List fusion | 10 named prelude stages | foldr/build ([Gill et al. 1993](https://dl.acm.org/doi/10.1145/165180.165214)); stream fusion ([Coutts et al. 2007](https://dl.acm.org/doi/10.1145/1291151.1291199)) | Any stage outside the list breaks a chain. `zipWith` matters most for DSP and pattern code. |
| Reuse | static linearity proof under a tracing collector | Perceus with dynamic refcount-1 reuse ([Reinking et al. 2021](https://www.microsoft.com/en-us/research/publication/perceus-garbage-free-reference-counting-with-reuse/)); FBIP in Koka and Lean | The static proof is defensible, since Perceus needs refcounting and conflicts with this collector. One non-linear use disables reuse. |
| Higher-order code | specialization by string key, with caps | MLton whole-program defunctionalization ([Cejtin et al. 2000](https://doi.org/10.1007/3-540-46425-5_4)) | The caps fall back to `fk_apply` with no message. The opt-report should name each refusal. |
| GC | conservative, non-moving, incremental SATB, stack copy-on-write | OCaml 5 (non-moving major heap, precise, deletion barrier); Go (hybrid barrier, assist pacer); Immix ([Blackburn and McKinley 2008](https://doi.org/10.1145/1375581.1375586)) | It is well matched to C interop and short pauses. Its costs are conservative retention, POSIX dependence and no assist. |
| Audio DSP | funk-compiled C kernels in a fixed graph | Faust (compiled DSP, many targets); SuperCollider UGens; cmajor | Checking against scsynth is a strong method. Faust already exports to CLAP, VST and the web, which funk does not. |

## 7. Recommendations

In order of value per unit of work.

Fix now (correctness, each small):

1. C2: the bound check in `tuple_name`.
2. C1: the value restriction for bindings that act, plus both repros.
3. C4: install signal handlers once; restore only your own.
4. C5: an audio-thread-safe CLAP `reset`.
5. C7: `waitpid` and `EINTR` handling.
6. C6: do not restore UDP from saved state.
7. Fix the stale docs listed in section 4.

Next (user access, before more parity work):

1. Multi-line input in `funk-midi`: `:{ :}` and blank-line-ended paragraphs, as the plugin has.
2. `make install` for `funk-midi` and `lib/`, with a lib path relative to the executable.
3. A quickstart that makes sound in 3 commands with `d1 $`.
4. `funk-midi --watch FILE`, or one editor package (VS Code or Neovim) that sends paragraphs.
5. R1, slots in the plugin.
6. Prebuilt macOS and Linux binaries of `funk-midi` and the `.clap` from CI.

Then (structure):

1. C3, C8, C9 and C10: session teardown, complete rollback, a per-chunk specialization budget, and per-chunk compiler arenas. These are all prerequisites for multi-hour sets.
2. Move pass sequencing out of check.c into one pipeline file. Split elab.c into its 4 passes. Add `FK_DUMP=<pass>` and per-pass switches.
3. Decide on a typed core IR between elab and the backends (section 8).
4. Commit the Params.hs generator for `lib/control.fk`. Give `pattern.fk` an export list.
5. Re-run `make bench`. Make real-time tests run serially, or check CPU time.

## 8. Alternative framings

- **A core IR instead of more passes.** A small typed core between elab and the backends would:
  - replace about 20 per-phase AST fields;
  - give one place for a simplifier, so fusion, specialization and reuse become rewrites instead of shape matches;
  - let each pass be tested alone;
  - let both backends share one input, which settles the interpreter question without dropping it.

  The cost is rewriting cgen.c and interp.c over the IR, about 1700 lines. This competes with 0.13 and Tidal parity for time.
- **"Tidal in your DAW" over "a Tidal replacement".** If the plugin is the product, the order changes:
  - R1, the plugin editor and binary `.clap` releases rise;
  - `funk-midi` usability and editor packages fall;
  - Windows becomes a requirement, since most DAW users are on Windows or macOS [I].
- **Engine, not language.** The 0.6 addendum lists Max/MSP, Pure Data and Eurorack as hosts. If funk is the embeddable engine for those, the priorities change again:
  - `libfunk.h` stability;
  - C3, C4 and C12;
  - a Windows runtime.

  Language growth (classes phase 2, comprehensions) then falls behind host safety.

Questions this review leaves open:

- Who is the first user other than the author, and which host do they use? The ordering in section 7 depends on the answer.
- How fast does a 2-hour live set grow compiler memory (C10) and use the specialization budget (C9)? One soak test answers both.
- Does the opt-report show the user-visible performance drops in real pattern code, or only in benchmarks?
