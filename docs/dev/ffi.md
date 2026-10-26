# Foreign function interface: design note

Status: proposal. Decided so far:

- Target libraries, in order: libm, sqlite3, libremidi.
- The interpreter may require a C compiler.

It depends on the typed-effects decision ([typed-effects.md](typed-effects.md)) for the types of effectful imports.

## Today

Primitives are fixed: `src/prims.def` lists them, `rt.c` implements them, and both backends call them. `funk build` runs `$CC` with fixed flags (`-O2 -lm -lpthread`), so a program cannot add a library. The interpreter has no way to load one.

## Designs

| | Design | Compiled | Interpreter | Glue |
|-|-|-|-|-|
| 1 | Stubs against the runtime API: the user writes `V f(V self, V *a)` in C with `rt.h` | links the stub file | `dlopen`s it; one signature, so no libffi | hand-written, against `rt.h` |
| 2 | Haskell's `foreign import ccall`, with automatic conversion | a direct call | libffi, or a table of call shapes | none |
| 3 | As 2, but the interpreter compiles the glue with `$CC` into a cached shared library | a direct call | the same glue as compiled code | none |

Chosen: 3.

- Both backends call the same C, so headers, macros and `static inline` functions work in both.
- It needs no new dependency: `funk build` already needs a C compiler, and the interpreter may now too.
- Design 1 would make `rt.h` a public ABI that must stay stable, with every binding written by hand.
- libffi, for design 2, cannot call macros, and a table of call shapes limits signatures.

## Syntax

Haskell's, so Haskell bindings port:

```haskell
foreign library "sqlite3"

foreign import ccall "math.h sin" c_sin :: Float -> Float
foreign import ccall "sqlite3.h sqlite3_open" sqliteOpen :: String -> Ptr (Ptr Db) -> IO Int
```

- **The entity string** is `"header.h cname"`. The header is required: a prototype derived from the funk type could disagree with the real one, and a call through a wrong prototype is undefined behaviour.
- **`foreign library "name"`** links `-lname` in `funk build`. The interpreter loads it as `libname.dylib` on macOS and `libname.so` on Linux. `funk build` also takes `-l` and `-L` options for libraries the source does not name.
- **The funk type** decides the conversion of each argument and of the result; see "Types".
- **`data Db`** with no constructors declares an opaque type for use in `Ptr Db`. Today's parser requires at least one constructor; this is a small extension.

## Types

| funk type | C type | In (funk to C) | Out (C to funk) |
|-|-|-|-|
| `Int` | `int64_t` | unboxed | boxed beyond 63 bits, as any `Int` |
| `CInt`, `CUInt`, `CLong`, `CSize` | `int`, `unsigned`, `long`, `size_t` | narrowed: a run-time error if out of range | widened |
| `Float` | `double` | unboxed | boxed, or raw in typed code |
| `CFloat` | `float` | narrowed | widened |
| `Char` | `char` | as is | as is |
| `Bool` | `bool`, or `int` as 0/1 | as is | non-zero is `True` |
| `()` | `void` | | `()` |
| `String` | `const char *` | the bytes in place: funk strings are NUL-terminated and never moved | copied into a new `String`; funk does not free the C string |
| `Ptr a` | `void *`, `T *` | the address | boxed in a new `K_PTR` object |

- **Narrow types are explicit.** Converting `Int` to `int` without a check would truncate silently.
- **`String` arguments are valid during the call only.** The collector does not scan C memory. A library that keeps the pointer must be given a copy (`newCString`, freed with `free`).
- **A `String` with a NUL byte inside** passes the bytes before it. That matches what C sees.
- **`Ptr a` needs a box,** because an odd address, such as a `char *`, has the `Int` tag bit set. `K_PTR` is not scanned, like `K_INT64`. Pointer equality and `nullPtr` are primitives.
- **Out-parameters,** such as `sqlite3_open`'s `sqlite3 **`, use a small prelude module: `malloc :: Int -> IO (Ptr a)`, `free`, `peekPtr`, `pokePtr`, `peekInt` and `pokeInt`, plus `withPtr`, which allocates a cell, runs an action and frees it. That is Haskell's `alloca` and `peek` in miniature.
- **Not in the first version:** structs by value, arrays, unions, variadic functions (`printf`).

## Effects

A foreign call can do anything. funk types effects with effect arrows ([typed-effects.md](typed-effects.md), option E), so an import says which it is:

- `foreign import ccall "stdio.h puts" c_puts :: String ->> CInt` may act.
- `foreign import ccall "math.h sin" c_sin :: Float -> Float` is pure. For a foreign function, `->` is the importer's promise, the one place an effect is not checked.

The optimizer treats an import written `->>` as acting, so fusion, eager `let` and reuse do not move it.

## Compiled backend

For each import, the generated program includes the header and emits a typed worker that converts its arguments and calls the C function. A call at known types then reaches the C function with no boxing: `c_sin x` is `sin(x)` on a `double`, which clang can inline. A wrapper with the uniform signature serves calls through `fk_apply`, as for every function ([typed-backend.md](typed-backend.md)).

## Interpreter

When a chunk declares foreign imports, the interpreter:

1. writes the glue: one `V fk_ffi_N(V self, V *a)` per import, converting from and to `V`, with the runtime's header embedded in `funk` the way `rt.c` is;
2. compiles it: `$CC -O2 -shared -fPIC glue.c -l... -o <cache>/<hash>.dylib`. The hash covers the glue text, the compiler and the flags, so a change rebuilds;
3. `dlopen`s it, `dlsym`s each `fk_ffi_N`, and binds the import to it as a primitive.

The first run of a program pays for the compile, probably 100-300 ms (an estimate); later runs load the cached library. The REPL does the same per chunk. The glue is the same C as the compiled backend's, so both backends agree on every conversion.

## Callbacks: libremidi

libremidi reports incoming MIDI messages through callbacks. From memory, and not checked against its headers: its C API (`libremidi-c.h`) takes a function pointer and a context pointer, and calls back on the backend's thread, CoreMIDI's on macOS. Two things follow.

- **A C function pointer per funk closure.** The glue emits one trampoline per callback type. The context pointer names a funk closure held in a root table, so the collector keeps it alive while C holds it. That is Haskell's `StablePtr`.
- **No funk code on a foreign thread.** The allocator and the collector are single-threaded. The trampoline therefore copies the message into a C queue and returns, without entering funk. funk reads the queue: a `waitEvent :: IO Event` that blocks, and a `pollEvent` that does not. Under B1, the runner could also drain the queue between actions.

A callback on the funk thread, such as `qsort`'s comparator, could call funk directly. But a funk runtime error there would `longjmp` across C frames, which is undefined behaviour for code like `qsort`. The first version therefore supports queued callbacks only. Direct callbacks would need errors stopped at the trampoline, which ends the program.

### Pauses

Real-time MIDI cares more about pauses than throughput. The collector stops the program for a whole mark and sweep: 6-15 ms at the benchmark sizes ([memory.md](memory.md#after-workers-and-fusion-2026-10-25)), against MIDI timing of about 1 ms. Queued input does not lose messages, because the trampoline never waits on the collector. Output sent from funk is delayed by any pause. libremidi therefore also needs one of the following, a separate design:

- lazy sweeping, which spreads the sweep over allocation;
- incremental marking;
- an allocation-free style for real-time loops, with a smaller heap.

### Threads

funk runs the program on a second thread with a 4 GiB stack (`fk_run`); the main thread waits on it. CoreMIDI does not need the main thread, as far as is known here. A library that does, such as Cocoa, would need the program's thread and the main thread swapped, or work posted to the main thread. That is out of scope until a target needs it.

## Phases

| Phase | Library | Needs | Estimate |
|-|-|-|-|
| 1 | libm | syntax; scalar conversions; compiled workers; interpreter glue and cache; `-l` | 400 lines |
| 2 | sqlite3 | `String` in and out; `Ptr` and `K_PTR`; opaque types; out-parameters; effects | 300 lines |
| 3 | libremidi | callbacks through queued trampolines; stable closures; context pointers | 400 lines, plus the pause work |

Estimates, not measurements. Windows `.dll` loading waits for a Windows port of the runtime, which uses `mmap` and `pthread` today.

## Tests

- **Phase 1:** `sin`, `pow` and `floor` through imports, in both backends, against today's primitives.
- **Phase 2:** an in-memory sqlite3 database (`:memory:`), with create, insert and select. Error paths: a bad SQL string, and a `String` with a NUL byte. Skipped when sqlite3 is not installed.
- **Phase 3:** a test library in the repo calls a registered callback from a thread it starts. This exercises the queue without a MIDI device. libremidi itself is tested by an example, not by `make test`.

## Questions

1. **Effects first?** The type of an effectful import depends on B1 or A''. Deciding first avoids changing the syntax of every effectful import later.
2. **Cache location:** `$XDG_CACHE_HOME/funk/ffi`, with `~/.cache/funk/ffi` as the fallback; or next to the source file?
3. **`Ptr` arithmetic and peeks at other types** (`peekFloat`, `plusPtr`): add them when a binding needs them, or provide Haskell's `Storable` set from the start?

Facts about libremidi's API are from memory and were not checked in this session.
