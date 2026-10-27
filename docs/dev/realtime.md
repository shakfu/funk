# Real time: design note

Status: R0-R4 implemented, released in 0.4.0; see the "Implementation" sections. The stack term of R2's start pause is removed ("The stack snapshot"). Open: generational collection (E), for throughput. It bounds the collector's pauses for any real-time use: audio, MIDI, games and control loops, not MIDI only. It replaces "Pauses" in [ffi.md](ffi.md#pauses).

## Requirements

| Use | Deadline | Thread | What a pause breaks |
|-|-|-|-|
| Audio | one buffer: 1.3 ms at 64 frames and 48 kHz, 10.7 ms at 512 | the audio API's, high priority | a dropout, audible |
| MIDI | about 1 ms of timing jitter | in: the backend's; out: funk's | late notes |
| Games, UI | a frame: 16.7 ms at 60 Hz, 8.3 ms at 120 Hz | funk's | a dropped frame |
| Control loops | the period: 1 ms at 1 kHz | funk's, or a timer's | a missed sample |

Two classes follow:

- **Soft real time, on funk's thread:** MIDI out, games, UI, most control loops. They need pauses well under their deadline, about 1 ms or less, at any heap size.
- **Hard real time, on a foreign thread:** audio callbacks, and any callback a driver calls with a deadline. The thread must not block or take a lock that funk holds, and it must not wait for a collection at all. No pause is short enough.

## Today, measured

An instrumented copy of the runtime timed each collection of two synthetic programs (`-O2`, M1, 2026-10-26). `churn` keeps a list of n cells live while a loop allocates garbage; `deep` allocates at the bottom of a recursion n frames deep. Each row is the longest collection of the run.

| Program | n | Roots (stack and tables) | Mark | Sweep | Pause |
|-|-|-|-|-|-|
| `churn` | 100,000 cells | 0.0 ms | 1.5 ms | 16.4 ms | 17.9 ms |
| `churn` | 1,000,000 cells | 0.0 ms | 10.1 ms | 24.8 ms | 35.0 ms |
| `churn` | 4,000,000 cells | 0.0 ms | 68.6 ms | 9.6 ms | 78.2 ms |
| `deep` | 1,000 frames | 0.0 ms | 0.0 ms | 14.8 ms | 14.8 ms |
| `deep` | 100,000 frames | 1.6 ms | 0.8 ms | 17.3 ms | 19.7 ms |
| `deep` | 1,000,000 frames | 16.4 ms | 6.8 ms | 11.9 ms | 35.0 ms |

- **Sweep:** 10-25 ms, about 0.1 ms per MiB of heap. The heap is at least 128 MiB, since a collection waits for that much allocation ([memory.md](memory.md#after-workers-and-fusion-2026-10-25)). It is the whole pause for small live data.
- **Mark:** about 10-17 ms per million live cells, so it grows with the program's state.
- **Stack:** about 16 us per 1,000 frames, scanned conservatively word by word.

Every pause is over every deadline in the table above; the shortest, 14.8 ms, is over all but the 60 Hz frame.

## What funk's semantics give

Values are immutable after construction. Only four code paths write into an object that already exists:

| Write | Where | Old value |
|-|-|-|
| a thunk's update with its value | `rt.c`, `fk_force` | the thunk's code and environment |
| TRMC's `*dst = v`, the tail of the last cell | `cgen.c` (`cg_tail`), `rt.c` (`split`, for `words` and `lines`) | a placeholder, or the old tail when the cell is reused |
| cell reuse, `FK_FIELD(cell, 0) = x` | `cgen.c`, from [reuse.md](reuse.md) | the dead cell's head |
| a recursive closure's free variables, filled after allocation | `cgen.c` (`closure_fill`), `interp.c` | nothing: the closure is new |

All other stores initialize a new object. The globals (`G`, `gvals`) and the `StablePtr` table are roots, written outside the heap.

The collector does not move objects, and C may hold pointers into the heap (`String` arguments, `StablePtr` contents), so a moving or copying design is out.

## Options

| | Design | Pause | Cost | Work |
|-|-|-|-|-|
| A | Lazy sweeping: allocation sweeps a chunk when its size class runs out | removes sweep: roots + mark | none measurable expected; sweep is spread, not removed | small |
| B | A, plus incremental marking: snapshot at the beginning, with a deletion barrier, in steps paced by allocation | roots + one step | a barrier at the four writes; a branch in allocation while marking | medium |
| C | B, plus incremental stack scanning (stack barriers) | one step | return-address patching; hard with conservative frames | large |
| D | Concurrent marking on a second thread | roots | B's barrier, plus a thread-safe mark state | large |
| E | Generational, non-moving (sticky mark bits) | frequent short young collections; full ones still pause | a barrier on old-to-young stores | medium |
| F | Reference counting (Perceus) | none, except cascading frees | measured slower: `lists` 136 ms against 70 ms ([memory.md](memory.md#perceus-prototyped-by-hand)) | large |
| G | Allocation-free real-time functions, exported to C | no interaction with the collector | a checked subset of funk | medium |

Notes:

- **B's barrier is cheap because funk mutates little.** Snapshot at the beginning keeps every object reachable when marking starts. Only overwrites can hide one, so only the four writes above log their old value, and initializing stores, nearly all of them, need nothing. Objects allocated while marking are allocated marked. An insertion barrier (Dijkstra's) would instead have to cover every initializing store into a new marked object, so it suits funk less.
- **The stack stays a pause in A and B.** Snapshot at the beginning scans the stack when marking starts: 16 us per 1,000 frames. Real-time code typically runs from a shallow event loop; a loop 1,000 frames deep pauses for under 0.1 ms for its stack. C removes this term but patches return addresses in frames the conservative scan cannot parse; it is not proposed.
- **D needs B's barrier anyway, and a second core.** It does not shorten the pause below B's roots term. Not proposed.
- **E lowers the collector's total cost, not its worst pause.** A full collection still marks the whole heap. It is a later complement to B, for throughput.
- **F is ruled out by measurement:** freeing scatters later allocations and costs locality ([memory.md](memory.md#perceus-prototyped-by-hand)).
- **No collector serves hard real time on a foreign thread.** A collection on funk's thread can free an object that only the audio thread's stack holds, since the collector scans one stack; and the audio thread cannot wait for funk's. G serves that class by never touching the heap.

## Recommendation

B for soft real time, G for hard real time, and two small supporting changes. In order:

1. **R0: pause statistics.** `FUNK_GCSTATS=1` prints each collection's roots, mark, sweep and pause times, and a summary at exit. Every later step is measured with it. Small.
2. **R1: lazy sweeping (A).** After marking, every chunk is unswept. Allocation that finds its free list empty sweeps the next unswept chunk of that size class, and a collection first finishes any sweep still pending. Expected: pauses of roots + mark, 1.5 ms and 10.1 ms for the first two `churn` rows. A hypothesis: total time unchanged.
3. **R2: incremental marking (B).** A collection becomes a cycle: scan the roots and push them (the pause), then mark in steps of a fixed budget, one step per k bytes allocated, then rescan the globals and the `StablePtr` table and finish (a second, short pause). The four writes call a barrier while a cycle is marking: `if (fk_marking) fk_log(old)`. Allocation while marking sets the new object's mark. Pacing starts a cycle early enough that marking ends before the heap must grow; k follows from the live size. Target: pause at most 0.5 ms plus 16 us per 1,000 frames of stack, at any heap size.
4. **R3: a lock-free event ring.** Queued trampolines write into a fixed-size ring of fixed-size events, one producer slot claimed with an atomic increment, instead of `malloc` and a mutex. A full ring drops the event and counts it. Byte buffers longer than an event's inline space (64 bytes, a parameter) fall back to `malloc`, or are dropped in a strict mode. This bounds the cost a callback adds to a foreign real-time thread.
5. **R4: allocation-free exports (G).** `foreign export ccall "realtime" process :: Ptr () -> Ptr CFloat -> CInt ->> ()` exports a top-level function as a C function with its typed worker's parameters, callable from any thread. The checker proves it allocation-free: its body uses scalar arithmetic, calls to other such functions, `peek` and `poke` at scalar types, and C imports; no constructors, closures, boxing or strings. State lives in C memory, reached through `Ptr`, and is exchanged with funk's thread through R3's ring. It needs unboxed `peek` and `poke` in the typed backend; today `peek` on a `Ptr CFloat` boxes its result (`fk_p_peekf` calls `fk_float`). The interpreter cannot run such an export, since it boxes every value; a program that uses one must be compiled.

R1 and R2 change only the runtime and four code-generation sites. R4 adds a checker pass and a typed-backend form, and is the only step visible in the language.

## Implementation: R0 and R1

- **R0.** `FUNK_GCSTATS=1` prints a line per collection and a summary at exit, in both backends and for `funk` itself:

  ```text
  gc 2: pause 8.68 ms (roots 0.01, mark 8.67, finish 0.00), live 9.6 MiB, heap 141 MiB
  gc: 2 collections, pause max 8.68 ms, mean 7.46 ms, total 14.93 ms; lazy sweep 248 chunks, max 0.087 ms, total 8.69 ms
  ```

  Times come from `clock_gettime` around each phase, read only when the variable is set. "Finish" is the large-object sweep and the rebuilding of the unswept lists. R2 changed the format; see "Implementation: R2".
- **R1.** As proposed in A, with three details:
  - A chunk left unswept when the next collection starts would keep its old marks, which read as already marked and would hide live objects. Its mark bytes are cleared instead, one `memset` of a byte per object; the new collection then frees what died since. Sweeping it would put its cost back in the pause.
  - The live size, which sets the next threshold, is counted while marking.
  - A chunk whose objects are all marked has nothing to free. Marking counts marked objects per chunk, and such a chunk only has its marks cleared. Without this, one allocation swept every full chunk of a long-lived list in a row: 3.1 ms at 4,000,000 live cells.

Pauses, the longest collection of each run, before and after (the programs of "Today, measured"):

| Program | n | Before | After | Longest lazy sweep step |
|-|-|-|-|-|
| `churn` | 100,000 cells | 17.9 ms | 0.98 ms | 0.14 ms |
| `churn` | 1,000,000 cells | 35.0 ms | 9.8 ms | 0.15 ms |
| `churn` | 4,000,000 cells | 78.2 ms | 37.5 ms | 0.15 ms |
| `deep` | 1,000 frames | 14.8 ms | 0.09 ms | 0.12 ms |
| `deep` | 1,000,000 frames | 35.0 ms | 23.0 ms | 0.10 ms |

What remains is marking and the stack, which R2 addresses. A sweep step costs at most one chunk, about 0.15 ms.

Throughput: the same emitted C, with lazy sweeping and with every chunk swept eagerly at the collection, 15 interleaved runs each, minimum wall time: `bst` 293.0 against 298.1 ms, `trees` 100.1 against 103.6, `sort` 63.6 against 69.1, `lists` 53.3 against 55.0. Lazy sweeping is 2-8% faster; the hypothesis was no change. A likely reason, not measured: an object is swept just before its reuse, so it is still in cache. The eager variant keeps the per-chunk mark count, so it is close to the old collector, not identical to it. The benchmark suite (`bench/results.jsonl`, runs of 2026-10-26 13:29 and 13:43) shows no change beyond noise.

Tests: `tests/run/gc.fk` collects several times while a 200,000-cell list stays live, in both backends, and the `gcstats` test checks the format of `FUNK_GCSTATS`'s output.

## Implementation: R2

As proposed in B, with these details:

- **A cycle.** The start pause clears the marks of chunks still unswept and scans the stack and the roots, pushing what they reach. Steps then mark for at most 0.4 ms each, one per 256 KiB of allocation (1 MiB until after 0.4.0; see "Step size" below); a step that empties the mark stack ends the cycle with the large-object sweep. While a cycle marks, every allocation takes the out-of-line path, which counts toward the next step and marks the new object; the inline path keeps no extra test.
- **Sweeping finishes before marking starts,** in steps of the same budget. A chunk unswept while marking cannot be swept until the cycle ends, since its marks are partial, so allocation took fresh chunks instead, and the heap grew by about 22 MiB a cycle without bound (`churn` at 1,000,000 cells: 160, 207, 231, 253, 276, 298 MiB). With sweeping finished first, every free object is on a free list when marking starts; the heap settles (217 MiB, 17 cycles).
- **The threshold** for the next cycle counts what marking found reachable, not the objects allocated during the cycle, which are mostly garbage by its end.
- **Barriers, three sites.** Snapshot at the beginning needs a write to shade the value it overwrites when that value could be the only path to an object a new, already marked object holds:
  - forcing a thunk clears its free variables, which its value may hold (`fk_force`);
  - a reused cell's head is overwritten (cgen);
  - `*dst` in a reuse copy may overwrite a reused cell's old tail (cgen). Other functions write `*dst` only over the placeholder, and cgen leaves the barrier out of them.

  `split`'s tails and closure filling write into new objects only, and the globals and the `StablePtr` table are roots scanned at the start.
- **Forced finish.** If a cycle has allocated twice its starting threshold, marking cannot keep up, and the step marks to the end in one pause. `FUNK_GCSTATS` counts these; none happened in the measurements below.
- **`FUNK_GCSTATS` output:**

  ```text
  gc 2: start 0.00 ms, 40 steps (max 0.42 ms), finish 0.02 ms, mark 16.21 ms in all, live 12.4 MiB, heap 200 MiB
  gc: 2 cycles, pause max 0.42 ms (start 0.02, mark step 0.42, finish 0.02, sweep step 0.25), total 23.15 ms, 0 forced; lazy sweep 204 chunks, ...
  ```

Pauses, the longest of each run:

| Program | n | R1 | R2 | Heap, R1 / R2 |
|-|-|-|-|-|
| `churn` | 100,000 cells | 0.98 ms | 0.40 ms | 138 / 147 MiB |
| `churn` | 1,000,000 cells | 9.8 ms | 0.40 ms | 160 / 214 MiB |
| `churn` | 4,000,000 cells | 37.5 ms | 0.40 ms | 289 / 491 MiB |
| `deep` | 1,000 frames | 0.09 ms | 0.06 ms | |
| `deep` | 100,000 frames | | 2.9 ms | |
| `deep` | 1,000,000 frames | 23.0 ms | 28.9 ms | |

The pause no longer depends on the heap; the stack remains, at about 29 us per 1,000 frames, now that the start pause also pushes what it finds. The heap is up to 70% larger: objects allocated during a cycle live until the next one, and the cycle's allocation comes on top of the threshold.

Throughput, compiled, 21 interleaved runs, minimum: against the same emitted C with barriers, the allocation test and the thunk loop removed and each cycle marked in one pause (close to R1), R2 measured -0.2% (`bst`) to +7.9% (`lists`): `trees` +1.8%, `sort` +3.7%, `wordfreq` +1.9%, `records` +3.0%. The largest figures are not the collector's work. Removing the thunk barrier's line alone took `lists` from 58.6 to 54.8 ms, but `lists` forces 80 thunks in all, so that line costs a few nanoseconds; the difference is code placement. The benchmark suite's run of 15:03 was taken under a load average of 4-5 (`usernoted` at 80% CPU); `bst` read 0.620 s there, and 0.303 s on a rerun, against 0.285 s before R1.

**Step size.** The `bst` figure above, 0.303 s against 0.285 s, was not load. Built at each commit since, `bst` slowed at the R2 commit and stayed 6-7% slower, in interleaved runs. Barriers and the out-of-line allocation path were not the cause: removing the barriers changed nothing, and marking new objects on the inline path recovered 1%. The cause is the heap's growth during a cycle. Nothing is freed until the cycle ends, so what the program allocates while marking comes from fresh chunks: with a step per MiB, `bst`'s second cycle took 36 steps, and its resident set grew from 151 to 207 MB, with 3,400 more page faults. A step per 256 KiB ends marking after a quarter of the allocation, with the same 0.4 ms bound:

| Program | Time, 1 MiB / 256 KiB | Resident set, 1 MiB / 256 KiB |
|-|-|-|
| `bst` | +6.1% / +1.6% against R1 | 195 / 152 MB |
| `churn`, 1,000,000 cells | | 229 / 185 MB |
| `churn`, 4,000,000 cells | | 547 / 359 MB |

The program's share of time while marking falls: `bst` allocates about 1 MiB per ms, so it runs about 0.25 ms per 0.4 ms step, against 1 ms before. 128 KiB recovered `bst` fully (+0.5%), at a share of about 25%. A pacer that sizes steps from the live set would bound the overshoot at any heap size; it is in TODO.

Tests:
- **`FUNK_GCSTRESS=1`** starts a cycle 64 KiB after the last one ends, marks 4,096 objects a step, never finishes a cycle early, and at each cycle's end re-traces everything reachable and stops the program if any of it is unmarked. Short, frequent cycles start inside the windows where a missing barrier loses an object; long, constant marking did not, since nearly every object was then allocated already marked.
- **`tests/run/gcbarrier.fk`** exercises the three sites. With the thunk barrier removed, the check stopped it in 3 runs of 3 and its output was wrong; likewise with the head barrier removed. With the `*dst` barrier removed it still passed: today's reuse copies overwrite only tails whose contents are garbage (`filter`'s dropped cells, `take`'s rest), so the barrier guards a shape no current function produces. It costs one test per cell in reuse copies only.
- **The check itself** was tested by marking one constructor in seven without tracing it: it stopped the program at the first cycle's end.
- The `stress` tests run `gc.fk` and `gcbarrier.fk` under `FUNK_GCSTRESS=1` in both backends. Every run test and example also passed under it, by hand.

## Implementation: R3

The queued-callback queue is a ring of 4096 slots of 128 bytes, a bounded queue in Vyukov's scheme: each slot's sequence number says whether it is free, being written or ready. A trampoline claims a slot with one compare-and-swap on the head, writes its arguments into the slot, and publishes it with a release store; no `malloc`, no lock. The 104 bytes after the slot header hold the arguments as a struct and then the bytes of `CStringLen` and `String` arguments; bytes that do not fit fall back to `malloc`, and if that fails the event is dropped. A full ring drops the event and counts it; `droppedEvents ()` returns the count.

`waitEvent` blocks on a semaphore that each publish signals: `dispatch_semaphore` on macOS, POSIX `sem_t` on Linux. Signalling without a waiter is an atomic increment on both, as far as their documentation states; with a waiter it is a system call. funk's thread frees a slot before it calls the closure, so a closure may itself call `waitEvent`. A producer that claimed an earlier slot may still be writing when a later one is published; the consumer yields until the earlier slot is ready.

Not measured: the time a trampoline takes. Tests: `tests/ffi/ring.fk` sends 5000 messages before funk reads any, and receives 4096, with 904 counted as dropped; and two 300-byte messages, which take the `malloc` path, with their contents checked. `callback.fk` and the libremidi example run unchanged.

## Implementation: R4

```haskell
foreign export ccall "fk_process" process :: Ptr () -> Ptr CFloat -> CInt ->> ()
foreign import ccall "fkt.h &fk_process" processPtr :: FunPtr (Ptr () -> Ptr CFloat -> CInt ->> ())
```

- **Exports.** `foreign export ccall "cname" name :: type`, Haskell's syntax, makes `cname` a C function that calls `name`'s typed worker. The type must be `name`'s, and made of C scalars, pointers and `()`. A value import of `&cname`, a C expression, gives its address as a `FunPtr`; the export's prototype precedes the glue. Exports need compiled code: the interpreter boxes every value, so `funk run` reports an export as an error.
- **The allocation check** runs after elaboration, on the export's function and every function it calls (`cgen_alloc_free`). It accepts literals; variables of scalar, pointer and `()` types; primitives that compile to C operators; C imports without `String`; full-arity calls to functions whose parameters and result are such types; `if`; `case` on scalars and `Bool`; and strict local values. It rejects constructors, closures, strings, lazy and recursive local bindings, unknown and partial calls, and other primitives, naming the expression and its line. Fusion and lambda lifting run first, so `sum (enumFromTo 1 n)`, a fused loop, and a lifted local function are accepted.
- **Pointers are C addresses in typed code.** `Ptr`, `FunPtr` and `StablePtr` have a representation of their own, `void *`, boxed only where they meet untyped code, like `Int`. `castPtr`, `plusPtr`, pointer equality and `peek` and `poke` at scalar types compile to C: `peek` on a `Ptr CFloat` was a call that boxed a `Float`. C imports with pointer arguments are now called directly.
- **Literals at a known `Num` instance** call the instance's `fromInt` directly: `0 :: CInt` passed the `Num CInt` dictionary before, a heap value, which the check rejected.
- **Errors.** A runtime error on a foreign thread aborts the program: funk's error handlers are on its own stack. The check therefore also rejects code that can fail: primitives that can (`div`, `mod`, `quot` and `rem` unless the divisor is a non-zero literal, `chr`, `floor`, `round`, `error`), and a `case` that does not cover every value (`case_covers`, as fusion uses it).
- **Enumerations.** A type whose constructors take no arguments, such as `Ordering`, is allocation-free: its values are static objects. Comparison through `compare` at `CLLong` returns one.
- **Not checked:** stack depth. An export runs on the caller's stack, which may be small, and generated code has no stack check.

The test, `tests/ffi/export.fk`, exports `process`, which fills a host's buffer from a counter kept in C memory. The test library calls it from its own thread, 2000 blocks of 64 samples, while funk's thread allocates and collects; the samples' sum, 3,168,000, and the counter, 128,000, are checked, also under `FUNK_GCSTRESS`. In the generated C, the 13 workers reachable from `fk_process` call none of the allocating functions (`fk_alloc`, `fk_mkcon`, `fk_float`, `fk_ptr`, `FK_MKINT`, `fk_closure`, `fk_thunk`, `fk_apply`, `fk_str`, primitive wrappers). Seven error tests cover a list, `show`, a `String` argument, a C name, the interpreter, a division by a variable and a `case` that does not cover every value.

### PortAudio

`examples/ffi/portaudio.fk` plays a sine through PortAudio 19.7.0 on the default output device: 48 kHz, one channel, 64-frame buffers, so a callback has 1.33 ms. The callback is a foreign export. funk's thread changes the frequency four times, and allocates meanwhile, enough for four collection cycles. The callback times itself with `clock_gettime` and counts underflows from PortAudio's status flags. Three runs of the first version, which computed `sin` per sample and took the frequency through `atomicPeek`, at gain 0 so as to make no sound (M1, 2026-10-26):

| Run | Callbacks | Underflows | Longest callback | Mean | Collection cycles during playback |
|-|-|-|-|-|-|
| 1 | 902 | 0 | 10 us | 1 us | 4 |
| 2 | 882 | 0 | 6 us | 1 us | 4 |
| 3 | 833 | 0 | 6 us | 1 us | 4 |

The second version takes frequency changes from a `Ring Float` and reads a 1024-entry sine table (`Array CFloat`) with `readArrayWrap`: in three runs, 892 to 902 callbacks, no underflow, the longest callback 4 to 5 us, the mean under 1 us. A first attempt turned the phase into an index by counting up to it, since `truncate` can fail: the mean was 28 us. `truncateSat`, a total conversion that saturates at `Int`'s range and maps NaN to 0, replaced it.

The longest callback took under 1% of its budget. The underflow count was 0, so the counting path ran but never counted; no run was overloaded to test it. Of the 31 workers reachable from `fk_synth`, none calls an allocating function; five contain a call to `fk_match_fail` or a zero check in `fk_s_div`, which the check proves unreachable. The example is not in `make test`, which does not open audio devices.

`Atomic` (`atomicPeek`, `atomicPoke`) has instances for `Int`, `Float` and the C types, and compiles to `__atomic_load_n` and `__atomic_store_n` with acquire and release order. funk has no atomic read-modify-write: the callback's counters work because one thread writes each.

## Arrays and rings

Both live in C memory, allocated by funk's thread and freed explicitly, so a foreign thread may use them: a collected array could be freed while the audio thread still reads it, since the collector does not see C's references. In typed code each is the address of its header, like a `Ptr`, so an export uses them without allocating.

- **`Array a`**, of `Storable` values: a header holds the length and the element size, from `sizeOf` (now in `Storable`, as in Haskell), and the elements follow. `newArray n x` allocates and fills; `readArray` and `writeArray` check the index and fail outside it; `readArrayWrap` and `writeArrayWrap` take it modulo the length, so they cannot fail and exports can use them, for wavetables and delay lines. `arrayLength`, `arrayPtr` (the elements, for C) and `freeArray`. An array has at least one element, so the modulus is never 0.
- **`Ring a`**, of `Storable` values, with one producer and one consumer, which may be any two threads. `head` and `tail` count pushes and pops; each side writes only its own, with release order, and reads the other's with acquire order. The capacity is rounded up to a power of two. The producer calls `ringPush`, which returns `False` when the ring is full; the consumer calls `ringCount`, then `ringFront` and `ringDrop`. Three calls, where Haskell would return a `Maybe`, because a `Just` is a heap object. `ringFront` on an empty ring returns an old slot rather than failing.
- **`Storable`** instances for `Array a` and `Ring a` store their address, so a callback's context can hold them.
- **`plusPtr`** now adds as integers: on a null pointer, C's pointer arithmetic is undefined, which UBSan reported for `plusPtr nullPtr 16`.

Tests: `tests/run/array.fk`, in both backends: indices checked and wrapped, a ring of capacity 3 taking four values, and arrays of pointers and `CFloat`. Two error tests: an index out of range, and a length of 0. `tests/ffi/control.fk`: funk pushes 50 gains into a ring before the test host starts; the export takes one per block of 2000, plays a 100-entry wavetable through `readArrayWrap`, and pushes every 100th block's number back through a second ring, while funk's thread collects. Its sample sum, 1564804.0014787447, matches a Python recomputation with the same float rounding; also under `FUNK_GCSTRESS`.

## The stack snapshot

R2 scanned the whole stack in a cycle's start pause, about 29 us per 1,000 frames. Snapshot at the beginning needs only the stack's contents at the start, not their scan, so the start pause now takes the snapshot cheaply and steps scan it:

- **Copy on write by page protection.** The start pause copies the words from the current frame to the next page boundary (16 KiB on an M1) and makes the older pages read-only with one `mprotect`. A write to one of them faults; the handler (`on_fault`) copies the page to a reserved area, makes it writable, and the write proceeds. Steps scan each page, from its copy if one was made, and make it writable again. For 96 MiB, about a million frames here, `mprotect` took 0.14 to 0.26 ms, against 9 to 12 ms for `memcpy`, which a first version used.
- **The kernel does not fault.** A system call that writes into a read-only page fails with `EFAULT` instead. The runtime reads input through stdio's heap buffer, but C code may pass its stack to the kernel (`read` into a local buffer). Two cases:
  - Before every foreign call on funk's thread (`fk_ffi_enter`, in the glue and before direct calls), the snapshot pages from the scan cursor up to the current frame are copied and made writable, since the C frames will go there once funk has returned above where the cycle started. The test is one load while no snapshot is active. A review found this case, which the first version missed; `tests/ffi/efault.fk` reproduces it, with `read` returning -1 without the release.
  - While a direct callback runs, the C frames below it are older than the cycle and already running; the start pause then copies the whole stack instead.
- **The thread's own data.** The pages are protected up to `task_main`'s frame, not to the top of the stack's mapping: glibc keeps the thread's control block and TLS there, which the kernel writes (from glibc's stack layout; not run on Linux). The few pages above are copied in the start pause.

Pauses, `deep` (allocation at the bottom of a recursion), longest of each run:

| Frames | R2 start pause | Now: start pause | Now: longest pause |
|-|-|-|-|
| 1,000 | 0.06 ms | 0.07 ms | 0.14 ms |
| 100,000 | 2.9 ms | 0.06 ms | 0.42 ms |
| 1,000,000 | 28.9 ms | 0.20 to 0.27 ms | 0.47 ms |

The start pause still grows with the stack, through `mprotect`'s cost per page, about 0.25 ms at a million frames, and 0.003 ms at 10,000. This is closed as a known bound: the snapshot must be taken at one instant, so every page is protected in the start pause, and removing the term would need return barriers, which conservative frames rule out. Real-time code runs from shallow event loops.

- **False reports by the check, found here.** `FUNK_GCSTRESS`'s check reported unmarked objects in `tests/run/gcdeep.fk`, reached from slots of frames made after the cycle started, below its stack pointer. Their unset slots held words of dead frames from before the cycle. The check clears the dead stack at each cycle's start, under `FUNK_GCSTRESS` only. A first version used `madvise(MADV_DONTNEED)`, which zeroes anonymous pages on Linux but not on macOS, so only the 32 KiB cleared with `memset` was zero and reports came from slots 41 to 82 KiB down; I first took them for a gap I could not explain and made the check skip such frames, which a review rightly flagged as weakening it. Mapping fresh pages over the dead stack (`mmap` with `MAP_FIXED`) zeroes it on both, and the check is strict again: the copy variant that failed 10 runs of 10 passes 20 of 20.
- Steps under `FUNK_GCSTRESS` scan the stack's snapshot a chunk at a time, not by the clock, so a stressed run repeats.
- **Tests.** `tests/run/gcdeep.fk` recurses 20,000 deep, keeping lists of lists in each frame and moving the inner lists into new cells on the way back, under `FUNK_GCSTRESS` in both backends; snapshot pages are written while read-only about 200 times a run. With the fault handler's page copy removed, the check stopped it in 3 runs of 3 while pages were protected up to the stack's top. With the limit below `task_main`'s frame, the outermost 16 KiB of frames, where the writes that matter fall in this program, are copied in the start pause, and the variant without the page copy passes: the page copy is now exercised but not proven necessary by a test. `tests/ffi/efault.fk` covers the release before foreign calls.

## Atomics, structs and stack depth

- **Read-modify-write.** `Atomic` gains `atomicAdd`, returning the old value, and `atomicCompareSwap`, sequentially consistent, for every instance: integers with `__atomic_fetch_add` and `__atomic_compare_exchange_n`, floats with a compare-and-swap loop on their bits. In `tests/ffi/export.fk`, the export and funk's thread each add 1,280,000 to a counter at once, and it ends at 2,560,000; with a plain read and write instead, one run in five lost updates, so the test can tell.
- **Structs.** `foreign struct "time.h struct timespec" Timespec { sec "tv_sec" :: CLong, ... }` takes C's struct from a header; `foreign struct State { freq :: CFloat, ... }` has the glue define one. Either way `State.freq :: Field State CFloat` is the field's offset, from `offsetof`, and `State.size` its size, so C lays it out. `readField`, `writeField` and `fieldPtr` use them; a `Field` is an `Int` in typed code, so exports may. For a header's struct, the glue asserts each field's type: the C type, or an enumeration of its size, for a scalar; a pointer, for a pointer. A string gives a member path, such as `"on_message.callback"` through libremidi's anonymous union. A field is a scalar, a pointer or another foreign struct, declared before it, held by value and reached with `fieldPtr`: `Timed { start :: Timespec, voice :: Voice }`, or `struct itimerval`'s two `struct timeval`s, checked by type. Each struct's C text has an include guard, and a struct's text holds those of the structs in it, so the glue has every definition a used field needs. C arrays inside structs, such as Core Audio's `AudioBufferList`, are not supported.
- **Stack depth.** The export check rejects recursion other than a call of the function itself in tail position, which compiles to a loop. The rest of the call graph has no cycles, so an export's stack use is bounded. The bound is not computed: frame sizes are the C compiler's, which `-fstack-usage` reports, so `funk build` could sum them along the call graph. Deferred until a target with small stacks needs it.

## Questions

1. **Is R4's restriction acceptable?** *(Built as proposed.)* Real-time audio code in funk would be a first-order, allocation-free subset: loops over `Ptr CFloat` buffers, with state in C memory. Real-time audio code in C avoids allocation, locks and I/O in the callback for the same reason ([Bencina, "Real-time audio programming 101"](http://www.rossbencina.com/code/real-time-audio-programming-101-time-waits-for-nothing)), but it is not idiomatic funk. The alternative, a region per callback, freed when the callback returns, would allow allocation inside a callback, but needs a check that nothing allocated there escapes.
2. **What pause target?** 0.5 ms plus the stack term covers MIDI, games and 1 kHz control loops. Audio relies on R4 either way.
3. **Order:** R0 and R1 are independent and small; R2 builds on R1. R3 and R4 are independent of the collector. Proposed: R0, R1, R2, then R3 and R4.

Generational collection (E) is left for throughput after R2.
