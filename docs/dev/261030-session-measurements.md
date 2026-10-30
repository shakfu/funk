# Session measurements, 2026-10-30

[embedding.md](embedding.md), phase 2: what a session per instance costs, before choosing how code reaches its state. Apple M1, macOS, Apple clang, `RelWithDebInfo`.

## How code reaches the session's state

The interpreter's hot state moved into one struct in scratch copies of the source: the allocator's bump pointers, free lists and counters, `fk_true`, `fk_false`, `fk_unit`, `fk_nil`, `fk_cons`, `fk_marking`, `fk_interrupt`, `fk_stack_limit` and `interp.c`'s `gvals`. Four builds of `funk`; `funk run` on the core benchmarks at `bench.py`'s sizes, builds in random order within each round. Median time against `base`:

| Build | The struct is reached through | Geomean | Range | Rounds |
|-|-|-|-|-|
| `base` | (globals, as today) | 1 | | |
| `global` | one plain global | 1.004, 1.006 | 0.95-1.03 | 7, 5 |
| `tls` | a `_Thread_local` pointer | 1.272 | 1.16-1.45 | 7 |
| `ptr` | a plain global pointer | 0.998 | 0.97-1.01 | 5 |

`tls` and the first `global` row cover 15 benchmarks; `ptr` and the second, 14, without `pipeline` (270 s a run interpreted).

- **`tls`**: on macOS every function that reads a thread-local calls its accessor through a pointer (`blr` in `fk_alloc`), which also gives it a stack frame. The interpreter is many small functions, each paying once.
- **`ptr`**: one load, no call. A pointer passed in a register costs at most this, so `ptr` stands for passing the session explicitly.
- Not measured: Linux, where a thread-local costs one instruction in an executable but a call to `__tls_get_addr` in a dylib by default (inferred); compiled programs, which keep a static struct in every design.

With the state behind a thread-local, `ffi.c`'s `glue_addrs` stops compiling: a table of the runtime's addresses cannot hold per-session ones.

## One session in a host

`funk_main`, `funk_init` and one `funk_eval` in a C program linking `libfunk.a`; resident size from `task_info`, malloc from `malloc_zone_statistics`; 3 runs, within 0.5 ms.

| Step | Resident | Time |
|-|-|-|
| `funk_init` | +9.9 MB, of which malloc +9.0 MB | 6.6 ms |
| first `funk_eval` | +0.3 MB | 9.4 ms |

A session is mostly the compiler's tables and the prelude's compiled code; the heap is under 1 MB. A frozen prelude shared by sessions would remove most of it.

## The allocator's fields

Step 4a moved `rt.c`'s statics into one static `FkRt`. Compiled `lists` (`bench.py ab HEAD`, 11 runs) ran 15% slower; `hyperfine` over 30 runs: 41.9 against 48.1 ms, 504M against 512M instructions, 125M against 144M cycles. Bisecting by declaration, with each group returned to separate statics: all of `FkRt`'s 45 lines together recovered nothing (142M cycles); `alloc`'s five fields alone (`freelist`, `bump`, `bump_end`, `alloc_since_gc`, `gc_threshold`) recovered all of it (124M). The same five in a struct of their own were as slow as in `FkRt`. Not the cause: `find_obj`'s inlining (`noinline` changed nothing), the binary's size. Inferred, not checked: in one struct the compiler cannot tell the arrays apart, so it reloads them around each allocation. The interpreter, measured with them in the struct, did not slow (1.004, above).

## Globals

288 writable symbols in `libfunk.a` (`nm -m`, `__DATA` data, bss and common): `rt.c` 107, `check.c` 64, `elab.c` 25, `driver.c` 23, `fuse.c` 17, `ffi.c` 13, `util.c` 11, `cgen.c` 10, five others 24. Function-local statics and compiler labels count; [embedding.md](embedding.md)'s 2026-10-27 count of about 215 did not.
