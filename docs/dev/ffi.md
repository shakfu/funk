# Foreign function interface: design note

Status: phase 1 (scalars, libm) implemented; see "Implementation (phase 1)". Phases 2 and 3 are proposals. Decided so far:

- Target libraries, in order: libm, sqlite3, libremidi.
- The interpreter may require a C compiler.
- Effectful imports are typed with `->>` ([typed-effects.md](typed-effects.md), option E).

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
foreign import ccall "sqlite3.h sqlite3_open" sqliteOpen :: String -> Ptr (Ptr Db) ->> CInt
```

- **The entity string** is `"header.h cname"`. The header is required: a prototype derived from the funk type could disagree with the real one, and a call through a wrong prototype is undefined behaviour.
- **`foreign library "name"`** links `-lname` in `funk build`. The interpreter loads it as `libname.dylib` on macOS and `libname.so` on Linux. `funk build` also takes `-l` and `-L` options for libraries the source does not name.
- **The funk type** decides the conversion of each argument and of the result; see "Types".
- **`data Db`** with no constructors declares an opaque type for use in `Ptr Db`. Today's parser requires at least one constructor; this is a small extension.

## Types

| funk type | C type | In (funk to C) | Out (C to funk) |
|-|-|-|-|
| `Int` | `int64_t` | unboxed | boxed beyond 63 bits, as any `Int` |
| `CInt`, `CUInt`, `CLong`, `CULong`, `CLLong`, `CSize` | `int`, `unsigned`, `long`, `unsigned long`, `long long`, `size_t` | in range by construction: arithmetic wraps | widened |
| `Float` | `double` | unboxed | boxed, or raw in typed code |
| `CFloat` | `float` | in range by construction | widened |
| `Char` | `char` | as is | as is |
| `Bool` | `bool`, or `int` as 0/1 | as is | non-zero is `True` |
| `()` | `void` | | `()` |
| `String` | `const char *` | the bytes in place: funk strings are NUL-terminated and never moved | copied into a new `String`; funk does not free the C string |
| `Ptr a` | `void *`, `T *` | the address | boxed in a new `K_PTR` object |

- **Narrow types are explicit.** Converting `Int` to `int` without a check would truncate silently. The prototype check makes them required: an `Int` cannot be declared where C has an `int`.
- **`String` arguments are valid during the call only.** The collector does not scan C memory. A library that keeps the pointer must be given a copy (`newCString`, freed with `free`).
- **A `String` with a NUL byte inside** passes the bytes before it. That matches what C sees.
- **`Ptr a` needs a box,** because an odd address, such as a `char *`, has the `Int` tag bit set. `K_PTR` is not scanned, like `K_INT64`. Pointer equality and `nullPtr` are primitives.
- **Out-parameters,** such as `sqlite3_open`'s `sqlite3 **`, use a small prelude module: `malloc :: Int ->> Ptr a`, `free`, `peekPtr`, `pokePtr`, `peekInt` and `pokeInt`, plus `withPtr`, which allocates a cell, runs an action and frees it. That is Haskell's `alloca` and `peek` in miniature.
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
- **No funk code on a foreign thread.** The allocator and the collector are single-threaded. The trampoline therefore copies the message into a C queue and returns, without entering funk. funk reads the queue: a `waitEvent :: () ->> Event` that blocks, and a `pollEvent` that does not.

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

## Implementation (phase 1)

Done 2026-10-26, as designed above, with these differences and details:

- **C types.** `CInt`, `CUInt`, `CLong`, `CULong`, `CLLong`, `CSize` and `CFloat` are built-in types, held as `Int` or `Float` words, so the typed backend unboxes them. Their `Num` instances wrap each result to the C width, as C and Haskell do; they are generated beside the tuple instances. `CULong` and `CSize` compare and show unsigned. Conversions go through `fromIntegral`, via a new class `Integral` with one method, `toInt`; Haskell's `Integral` has `toInteger` and the division methods instead. `CFloat` converts with `toCFloat` and `fromCFloat`, since funk has no `Fractional` class or `realToFrac`; a `CFloat` literal is written `toCFloat 1.5`. Values pass in range, so narrowing at the call never truncates. This replaces the run-time range check of "Types": arithmetic wraps, and only `fromIntegral` can produce an out-of-range input, which it wraps too.
- **The prototype check.** The glue asserts, for each import, `__builtin_types_compatible_p(__typeof__(cname), <declared C type>)`. A declaration that differs from the header in a width, a sign, the arity or the result is a compile error from the C compiler, with clang naming both types: "requirement '__builtin_types_compatible_p(int (int), long long (long long))': foreign import c_abs :: Int -> Int needs function `abs` to have the C type int64_t (int64_t)". On Linux `int64_t` is `long`, so a C `long long` needs `CLLong` there and on macOS alike. A function-like macro has no type; the check is skipped (`#ifndef cname`), and C converts the arguments. glibc's `isdigit` is such a macro, macOS's an inline function. The builtin needs gcc or clang.
- **`()` as an argument** is allowed only as the sole one: `rand :: () ->> CInt` calls `rand()`.
- **C values.** An import of a non-function type reads a C expression once, at startup: `"math.h M_PI" piC :: Float`, checked like a function. Haskell imports addresses with `&` instead; that waits for `Ptr`. A variable such as `errno` is read once too, so this is for constants.
- **Effects.** `->` marks a pure import, as a primitive's type does, and fusion treats its calls as free of effects, though not of failure. `->>` imports count as acting.
- **Compiled calls** at full arity are direct: `c_sin x` in a typed worker is `sin(t2)` on a `double`. A loop of 10^7 calls ran in 0.448 s, against 0.446 s with the `sin` primitive. Partial applications and calls through `fk_apply` use the wrapper `fk_ffi_N`.
- **The interpreter's glue** is `rt.h`, embedded in `funk`, followed by the wrappers. It is built with `$CC -O2 -shared -fPIC`, plus `-undefined dynamic_lookup` on macOS, and links the program's `foreign library` names and `-lm`. The runtime functions it calls resolve against `funk` itself, which CMake builds with exported symbols (`ENABLE_EXPORTS`). The glue compiled in 87 ms warm, against 1.0 s on a cold first start of the compiler. With the library cached, a whole run of a small program took 18 ms.
- **The cache** is `$XDG_CACHE_HOME/funk/ffi`, or `~/.cache/funk/ffi`, named by a hash of the glue text, the `$CC` words, the libraries and the command-line `-I`, `-L` and `-l`. A library is written to a temporary name and renamed, so concurrent runs do not read half a file. A hit refreshes the library's mtime, and each compile deletes entries older than 30 days. `make test` points the cache at its temporary directory.
- **A compile failure** prints the compiler's messages and the glue's path. In the REPL, the chunk's imports are then undefined, and the session continues.
- **Search paths.** `funk run`, `repl` and `build` take `-I`, `-L` and `-l`, for headers and libraries outside the compiler's defaults, such as Homebrew's.

Tests:
- `tests/run/ffi.fk`, in both backends: `sin`, `pow` and `floor` against the primitives; a 64-bit value through `llabs`; `ctype.h` through `CInt`; `sinf`; the constants `M_PI` and `INT_MAX`; `rand` and `srand` in order.
- `tests/run/ctypes.fk`: wrapping, unsigned comparison and `show`, and `CFloat` digits.
- `tests/ffi/link.fk`: a test library in `tests/ffi/`, built by `tests/run.sh` and found with `-I`, `-L` and `-l`, in both backends.
- Eight error tests (`tests/errors/ffi_*.fk`): an unsupported type, `()` among other arguments, a `()` value, the entity string, the calling convention, a prelude name, a `->>` import called under `->`, and a type that differs from the prototype.

## Tests

- **Phase 1:** `sin`, `pow` and `floor` through imports, in both backends, against today's primitives. Done; see above.
- **Phase 2:** an in-memory sqlite3 database (`:memory:`), with create, insert and select. Error paths: a bad SQL string, and a `String` with a NUL byte. Skipped when sqlite3 is not installed.
- **Phase 3:** a test library in the repo calls a registered callback from a thread it starts. This exercises the queue without a MIDI device. libremidi itself is tested by an example, not by `make test`.

## Questions

1. ~~**Effects first?**~~ Decided: option E; effectful imports use `->>`.
2. ~~**Cache location.**~~ `$XDG_CACHE_HOME/funk/ffi`, with `~/.cache/funk/ffi` as the fallback. A cache next to the source would leave files in source trees, and the REPL has no source file.
3. ~~**Narrow types.**~~ Real funk types with `Num` instances, as Haskell has, over names valid only in foreign signatures; see "Implementation (phase 1)".
4. **`Ptr` arithmetic and peeks at other types** (`peekFloat`, `plusPtr`): add them when a binding needs them, or provide Haskell's `Storable` set from the start?

Facts about libremidi's API are from memory and were not checked in this session.
