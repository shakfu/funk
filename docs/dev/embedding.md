# Embedding: multi-instance scope

Status: scope, 2026-10-27, of funk 0.6.0; phase 2 designed 2026-10-30. Hosts: Max, Pure Data, VST3/AU/CLAP plugins, Eurorack, [psnd](https://github.com/shakfu/psnd) ([261027-lang-review.md](261027-lang-review.md#addendum-target-users-and-hosts)). It asks what a host needs to run several funk programs in one process.

**Recommendation**, revised 2026-10-30. Phase 0 is done. Do phase 2 next: a session per instance, its state passed as an explicit pointer ([261030-session-measurements.md](261030-session-measurements.md)). Defer phase 1, namespaces in one runtime, to hosts with hundreds of small instances. Phase 1 was first because phase 2 seemed to slow compiled code; compiled programs keep a static struct, so it does not. Phase 1 also shares one heap limit and one collector across instances, and its main unknown, an undo log per instance, is still open.

## What is global today

Writable file-scope symbols, counted with `nm` on `build/projects/funk/CMakeFiles/libfunk.dir/src/*.o`. A vector appears as three symbols, so counts are approximate.

| Layer | Files | Writable globals | Holds |
|-|-|-|-|
| Runtime | `rt.c` | ~75 | heap and chunks, free lists, bump pointers, GC cycle state, stack snapshot, stables, export ring, constructor table, GC stats |
| Compiler | `check.c` ~55, `elab.c` ~23, `driver.c` ~18, `fuse.c` ~16, `cgen.c` ~7, `util.c` ~7, `interp.c`, `resolve.c`, `parse.c` ~4 each | ~140 | type and class maps, inference counters, prelude, loaded modules, intern table, undo log, interpreter global values |

Process-wide resources outside those globals:

- `task_main` installs `SIGSEGV` and `SIGBUS` handlers (`projects/funk/src/rt.c`), used by the stack snapshot's write barrier and the stack guard.

- `fk_run` reserves a 4 GiB stack and starts its own thread. `heap_reserve` claims 64 GiB of address space; `gc_min` is 128 MiB.

- 17 calls to `exit()`: 13 in `rt.c`, 4 in `util.c`. Stack overflow calls `_exit(1)`.

- `funk_build` writes executables only. `-shared` is used only for foreign-import glue (`projects/funk/src/ffi.c`).

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

Deferred, 2026-10-30, behind phase 2; see the recommendation.

One runtime per process: one heap, one funk thread, one compiler. An instance is a namespace in it. Pd runs all objects on one thread; Max's calls can be queued onto funk's thread; real-time exports stay off it.

1. **Instance = module prefix.** `qualify.c` already prefixes a module's names. Add `funk_instance_new` and `funk_instance_free`; freeing drops the instance's global values and stables for the collector.

2. **Reload = redefine within an instance.** Check whether the undo log (`undo_mark`, `undo_to`) can roll back one instance's definitions alone.

3. **Queue host calls onto funk's thread.** The ring carries exports to funk's thread; hosts need the reverse direction, with results by callback. Today `fk_ffi_enter` aborts off funk's thread. `funk_call` (2026-10-28, [patterns.md](patterns.md)) is the synchronous form, for a host already on funk's thread; the queue would carry its calls.

4. **Serialize compiles with a mutex.** The compiler's globals then stay.

Acceptance test: a Pd external with two instances running different code, one reloaded while the other plays.

## Phase 2: sessions

A `funk *` on every call; nothing mutable shared between sessions but a process registry.

### API

```c
typedef struct funk funk;
typedef struct {
  size_t heap, threshold, stack; /* 0 keeps a default */
  const char *const *cc_args; int ncc_args;
} funk_config;

funk *funk_new(const funk_config *c); /* starts the session's thread, compiles the prelude */
void funk_free(funk *f);
int funk_run(funk *f, int (*fn)(funk *, void *), void *arg); /* fn on f's thread */
/* any thread: funk_interrupt, funk_output, funk_on_fatal; the rest, as
   before with f first, inside funk_run */
```

`funk_new` and `funk_run` replace `funk_main` and `funk_init`; `funk_config`, `funk_memory`. `funk_output` and `funk_on_fatal` stay setters, since hosts change them while a session runs (`embed/output`, `embed/memory`).

### Threads

Each session owns a thread, on the runtime's large stack; a host thread has 512 KB to 8 MB, and the collector and stack-overflow check need the guarded one. `funk_run` from another thread queues its job and waits; from `f`'s thread it runs at once (phase 1's item 3). Any other call off `f`'s thread fails with a message rather than queueing, so a host sees each crossing. A session is never on two threads at once, so it takes no locks. Over the host entering a session on its own thread: that needs a stack switch per entry, which is what `funk_main` did.

### State

| Kind | Examples | Where |
|-|-|-|
| Fixed for the process | `fk_prims`, `page_size`, the builtin nullaries | global, `const` or set once |
| Runtime | heap, free lists, bump pointers, collector, stables, export ring, `fk_cons`, stack guard, `fk_error_jmp`, interrupt flag, callbacks | `FkRt`, in `funk` |
| Compiler | `check.c`'s tables, `elab.c`, `driver.c`'s modules and prelude, the undo log, `gvals`, `ffi.c`'s provided names and linked glue | `FkCompiler`, a part per file, in `funk` |
| Function-local scratch | `fk_p_getline`'s buffer, `verify_marks` | moved into the structs |
| Process-wide by nature | `SIGSEGV`/`SIGBUS` handlers, the live sessions | the registry |

- `fk_true`, `fk_false`, `fk_unit` and `fk_nil` become static objects, never collected, as compiled programs already have them (`fk_nullaries`).
- The registry installs the signal handler with the first session and removes it with the last. The handler finds the faulting session by address, its snapshot and stack guard, and passes other faults on. Each session saving and restoring the previous handler breaks when sessions end out of order.

### Reaching the state: an explicit pointer

Measured: a thread-local pointer costs the interpreter 27%, a pointer by load nothing ([261030-session-measurements.md](261030-session-measurements.md)). So, as Lua passes `lua_State *`, runtime and compiler functions take the session:

- The API's `funk *` reaches the compiler and runtime.
- An interpreted closure's `Lambda` holds its session, so `interp_entry` finds it with one load; the closure calling convention, shared with compiled code, does not change.
- Primitives and glue wrappers called from the interpreter take it as an argument.
- The signal handler finds it through the registry; an export's callback through its closure.

Compiled programs are one session: the runtime pasted into them uses one static `FkRt`, at a constant address, so they cost what they do now.

A global pointer switched on entry would also cost nothing, but lets one session run at a time, behind a process lock; one long evaluation then stops every other session.

### Glue

`ffi_load` writes the runtime's addresses into each glue `.so` (`GLUE_NAMES`). Two sessions loading one cached `.so` share its image, so the second's addresses replace the first's. The functions stay process-wide; `fk_true` and its kin become the static objects; `fk_cb_failed`, `fk_callback_depth`, `fk_snap_lo` and `fk_error_jmp` are reached through the session argument. One image then serves every session.

### Steps

Each passes `make test`.

1. Measure. Done, 2026-10-30.
2. The API alone: `funk *` in `libfunk.h`, backed by the one global session; the CLI, `funk-midi`, the plugin and `embed/*` move to it. Done, 2026-10-30, with the session's thread (from step 6); test `embed/session`. A process makes one session in its life until step 4.
3. Static builtin nullaries; `const` on fixed tables.
4. `FkRt` and the session argument in the runtime and interpreter; glue binding. Benchmarks against step 3.
5. `FkCompiler`, a file at a time, `check.c` first.
6. The registry; the session's thread is done.
7. Tests: `embed/sessions`, two sessions on two threads defining `x` differently, evaluating and collecting at once, freed out of order, also under TSan; `embed/globals`, `nm`'s writable symbols against an allow-list, so a new global fails.

## Unchecked

- Phase 1: whether the undo log can be split by instance, whether `gvals` can be split by namespace, and how much of `driver.c`'s module state (`mods`, `mod_chunks`) assumes one root program.

- Phase 2: the cost of the session argument in compiled programs, if they ever take it; Linux's thread-local cost; whether the queue onto a session's thread adds more than tens of microseconds per call.
