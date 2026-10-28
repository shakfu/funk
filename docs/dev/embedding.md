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

1. **Chain signal handlers.** Save the previous `sigaction` and forward faults outside funk's ranges. Today such a fault resets to `SIG_DFL`, and a second runtime replaces the first's handler, so the first's snapshot writes crash. DAWs often install their own crash handlers.
2. **Never end the host process.** Turn the `exit()` calls and the stack-overflow `_exit` into errors at the `libfunk.h` boundary.
3. **Output and error callbacks** in `libfunk.h`, in place of stdout and stderr, so `putStrLn` reaches the Max or Pd console.
4. **Memory sizes as `funk_init` parameters:** heap reserve, `gc_min`, stack size. A 128 MiB floor before the first collection is too large for Eurorack, and likely for plugins.
5. **Shared-library output** (`funk build --shared`), with runtime symbols hidden. Otherwise two funk-built plugins may resolve each other's `fk_cons` and `fk_true`. Inferred from the `extern` declarations in `rt.h`; untested.

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
- **Partly covered by phase 0 item 5:** each funk-built `.so` has its own private runtime, which covers two different funk plugins in one DAW.

## Unchecked

- Whether the undo log can be split by instance (phase 1 item 2): the main unknown in phase 1.
- Whether `interp.c`'s `gvals` can be split by namespace, or needs one table per instance.
- How much of `driver.c`'s module state (`mods`, `mod_chunks`) assumes one root program.
