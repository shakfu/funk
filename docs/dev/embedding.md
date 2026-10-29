# Embedding: multi-instance scope

Status: scope, 2026-10-27, of funk 0.6.0. Hosts: Max, Pure Data, VST3/AU/CLAP plugins, Eurorack, [psnd](https://github.com/shakfu/psnd) ([261027-lang-review.md](261027-lang-review.md#addendum-target-users-and-hosts)). It asks what a host needs to run several funk programs in one process.

**Recommendation.** Do phase 0 (embedding basics) and phase 1 (instances in one shared runtime). Defer phase 2 (separate sessions) until a host needs isolated heaps.

## What is global today

Writable file-scope symbols, counted with `nm` on `build/CMakeFiles/libfunk.dir/src/*.o`. A vector appears as three symbols, so counts are approximate.

| Layer | Files | Writable globals | Holds |
|-|-|-|-|
| Runtime | `rt.c` | ~75 | heap and chunks, free lists, bump pointers, GC cycle state, stack snapshot, stables, export ring, constructor table, GC stats |
| Compiler | `check.c` ~55, `elab.c` ~23, `driver.c` ~18, `fuse.c` ~16, `cgen.c` ~7, `util.c` ~7, `interp.c`, `resolve.c`, `parse.c` ~4 each | ~140 | type and class maps, inference counters, prelude, loaded modules, intern table, undo log, interpreter global values |

Process-wide resources outside those globals:

- `task_main` installs `SIGSEGV` and `SIGBUS` handlers (`src/rt.c`), used by the stack snapshot's write barrier and the stack guard.

- `fk_run` reserves a 4 GiB stack and starts its own thread. `heap_reserve` claims 64 GiB of address space; `gc_min` is 128 MiB.

- 17 calls to `exit()`: 13 in `rt.c`, 4 in `util.c`. Stack overflow calls `_exit(1)`.

- `funk_build` writes executables only. `-shared` is used only for foreign-import glue (`src/ffi.c`).

Real-time exports already support many instances: they do not touch the heap, and state arrives through a `Ptr` argument ([realtime.md](realtime.md), R4). The blocked part is funk's own thread: patterns, reload, control.

## Phase 0: embedding basics

Needed by every host, whatever follows.

1. **Chain signal handlers.** Done, 2026-10-28: `pass_on` in `rt.c`; test `embed/signals`. A host's handler installed before funk's gets the faults that are not funk's; one installed after funk's must pass funk's on, or the stack snapshot's write barrier breaks. When `funk_main` returns, the replaced handlers are installed again.

2. **Never end the host process.** Partly done, 2026-10-28: a full heap is the runtime error "out of memory", after a full collection, so a host recovers (test `embed/memory`). Two exits stay, by decision:
   - **`malloc` returning NULL**, in the compiler, the runtime's tables and the collector's mark stack and stack copy. It runs `funk_on_fatal`'s hook, then exits. On macOS, and on Linux with its default overcommit, `malloc` of these sizes does not fail: the system swaps or kills the process first (from the platforms' documented behaviour, not measured). Making the collector's buffers unable to fail means reserving them up front, the mark stack as large as the heap's limit; not worth it for a failure that does not occur.

   - **Stack overflow in compiled code**, which exits from the signal handler. No host runs compiled code yet. A recovery would leave the handler by `siglongjmp`, which is unsafe when the overflow is inside libc, such as `malloc` holding its lock, or inside the collector. Decide with item 5, when a host first runs compiled code. Interpreted code reports overflow as an error already.

3. **Output and error callbacks.** Done, 2026-10-28: `funk_output` in `libfunk.h`; test `embed/output`. The C compiler's output for foreign-import glue still goes to stderr.

4. **Memory sizes.** Done, 2026-10-28: `funk_memory(heap, threshold, stack)` before `funk_main`; test `embed/memory`. The threshold stays at most a quarter of the heap's limit. Empty chunks are reusable by any size class; those beyond the threshold go back to the system, and after "out of memory" a full collection runs at once, so the heap falls back. In the CLAP plugin, a 512 MiB limit reached left the host at 217 MiB, against 589 MiB before.

5. **Symbol isolation.** Done, 2026-10-29, but for `funk build --shared`:
   - `libfunk` is built with `-fvisibility=hidden` and exports only `libfunk.h`'s `funk_*` (test `embed/exports`). `make install` installs `libfunk.a`, `libfunk.h` and a CMake package, `find_package(funk)` giving `funk::libfunk` (test `embed/package`).

   - Foreign-import glue reaches the runtime through pointers that `ffi_load` sets after loading it, not through symbols (`GLUE_NAMES` in `ffi.c`; test `embed/plugin`). Measured on macOS before the change, with two modules each embedding `libfunk`: loaded `RTLD_LOCAL`, or exporting only an entry point as a CLAP plugin does, glue failed to load (`symbol not found in flat namespace`); loaded `RTLD_GLOBAL`, the second module's glue bound the first's runtime. The glue's cache key includes the path of the image that loads it, since two plugins loading one file would share one image and one set of pointers.

   - A host's own functions and variables reach imports the same way, through `funk_provide` (test `embed/plugin`, with a `static` function). `funk-midi` provides `funkgraph`'s functions to `lib/graph.fk`'s imports, and no longer exports its symbols.

   - **`funk build --shared`**, a library with its own private runtime. Deferred: `funk emit-c --kernels` covers a compiled synth inside an interpreting host ([audio.md](audio.md), D) without a second runtime. A full runtime per library needs an entry point that starts its thread, 64 GiB of address space and a 4 GiB stack each, and a decision on item 2.

6. **Interrupts.** Done, 2026-10-28: `funk_interrupt`; test `embed/interrupt`. Needed for a host's Stop; not in the first list.

## Phase 1: instances in one runtime

One runtime per process: one heap, one funk thread, one compiler. An instance is a namespace in it. Pd runs all objects on one thread; Max's calls can be queued onto funk's thread; real-time exports stay off it.

1. **Instance = module prefix.** `qualify.c` already prefixes a module's names. Add `funk_instance_new` and `funk_instance_free`; freeing drops the instance's global values and stables for the collector.

2. **Reload = redefine within an instance.** Check whether the undo log (`undo_mark`, `undo_to`) can roll back one instance's definitions alone.

3. **Queue host calls onto funk's thread.** The ring carries exports to funk's thread; hosts need the reverse direction, with results by callback. Today `fk_ffi_enter` aborts off funk's thread. `funk_call` (2026-10-28, [patterns.md](patterns.md)) is the synchronous form, for a host already on funk's thread; the queue would carry its calls.

4. **Serialize compiles with a mutex.** The compiler's globals then stay.

Acceptance test: a Pd external with two instances running different code, one reloaded while the other plays.

## Phase 2: separate sessions (deferred)

Move the runtime's globals into a `Runtime` struct and the compiler's into a `Compiler` struct.

- **Cost.** The compiler half is mechanical, mostly in `check.c`. The runtime half reaches generated code, which uses `fk_cons`, `fk_true`, `fk_nil`, the bump allocator and `fk_marking` directly. Each allocation then needs a context pointer or a thread-local lookup. Thread-local access in a macOS dynamic library calls `__tlv_get_addr` (from memory, not measured), which risks the 1.18x-of-C result. Needs interleaved A/B timing first.

- **Needed when** instances need independent collector pauses, or a host runs funk on several threads in parallel.

- **Partly covered by `funk build --shared`** (phase 0 item 5, deferred): each funk-built `.so` would have its own private runtime, which covers two different funk plugins in one DAW.

## Unchecked

- Whether the undo log can be split by instance (phase 1 item 2): the main unknown in phase 1.

- Whether `interp.c`'s `gvals` can be split by namespace, or needs one table per instance.

- How much of `driver.c`'s module state (`mods`, `mod_chunks`) assumes one root program.
