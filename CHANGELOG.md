# Changelog

Format: [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [Unreleased]

### Added

- `lib/pattern.fk`, a pattern library imported with `-M lib` ([docs/dev/patterns.md](docs/dev/patterns.md)). A pattern is a query from a span of time to the events overlapping it, as in Tidal. Time is in integer ticks, 720720 per cycle, rather than rationals: arithmetic stays in `Int`, and subdivisions up to 16 are exact. `sampleOnsets`, `pushOnsets` and `writeOnsets` turn a block of samples into onsets for an export's ring or a host's buffer.

- `funk_call` in `libfunk.h`: calls a global from `Int`s to `Int` without compiling. A host that called `funk_eval` once per audio window grew by about 16 KB per call, since every REPL input stays compiled; `funk_call` allocates nothing in the compiler.

## [0.6.0] - 2026-10-27

### Added

- GitHub Actions CI (`.github/workflows/ci.yml`): `make test` on Linux and macOS, plain and with funk built under UBSan.

- Modules, phase 1 ([docs/dev/modules.md](docs/dev/modules.md)). `import a.b` compiles `a/b.fk`, found beside the importing file or under `-M DIR`, and makes its names available qualified by the last segment, or by the name after `as`: `b.f`, `b.T`, `b.C`, `x b.<+> y`. Qualified-only, since a name added to a module then cannot clash in the files that import it. Every module is compiled once per program, and whole-program optimization still crosses module boundaries: a module's list pipeline fuses as before.

  ```haskell
  import shapes.geometry
  main = print (geometry.area (geometry.Circle 2.0))
  ```

- Import and export lists, modules phase 2. `import m (f, T(..), C(..), (<+>))` also brings those names in unqualified. `module m (f, T, C(..)) where` exports only those. A type exported without `(..)` is abstract: its constructors and fields stay inside its module, and its namespace functions go with it. A type not exported keeps its fields and namespace functions inside even when its values leave the module.

- Modules phase 3: `import` at the REPL prompt, and `:l` puts a file's imports in the REPL's scope. Haskell's `module Main (main) where` and base imports are accepted: `import Data.List (sortBy)` and `import qualified Data.Char as C` check their names against the prelude, which stands in for those modules; `C.ord` is `ord`.

- `bench.py ab OLD [NEW]`: compares two compilers by running their binaries alternately, which cancels the drift of about 5% between sessions (`bench/README.md`).

### Changed

- `pipeline` runs at n = 100,000,000, 0.3 s, up from 300,000, 3-6 ms, which was too short to time; old rows are not compared. `report` judges only funk's rows, since nothing in funk changes C's.

- regex-redux uses PCRE2, not PCRE, which Homebrew has deprecated and recent Linux releases drop: funk through foreign imports, and the Game's PCRE2 C program (`gcc-5`), built without OpenMP. Homebrew's PCRE had no JIT on arm64 (`gcc-2` took 1.50 s on fasta's output for 1,000,000, `gcc-5` 0.56 s); funk takes 0.58 s.

### Fixed

- An effectful function could not call a parameter declared pure: `each :: (Int -> ()) ->> ()` with `each f = f 1` was rejected, since the call merged the signature's shared `->` effect into the body's `Eff`. A body that is already `Eff` takes a call of any effect now (`docs/dev/typed-effects.md`).

- The effect error for a signature's `->` names the arrow, by the arguments before it: "its signature's arrow after 2 arguments is `->`; it needs `->>`". A returned function that acts at a `->` reports "where its signature has `->`".

- `show` parenthesizes `-0.0` as an argument, as GHC does: `Just (-0.0)`, not `Just -0.0`.

Found by the first Linux run:

- The compiler passed NULL to `memcpy` and `memmove` with a size of 0, from empty arrays, in 24 places; undefined in C, and reported by UBSan against glibc's `nonnull` declarations, which macOS's headers lack. They go through `copy_n` and `move_n` now.

- Generated C failed GCC's `-Wall -Werror`: a worker that only loops had no `return` (`-Wreturn-type`; each ends in `__builtin_unreachable()`, with unchanged machine code), and `fk_ptr` took a `const void *`, from which GCC concluded that `mallocBytes`' fresh memory was read uninitialized.

- `tests/run/overflow.fk` recursed through `1 + 2 * _`, which GCC turns into a loop with an accumulator, so it ran 2 billion steps instead of overflowing; it uses `mod` now.

- The reuse and fusion tests sorted their lists by the locale; they sort by bytes (`LC_ALL=C`).

## [0.5.0] - 2026-10-27

### Added

- A second benchmark suite, `bench/game/`: the Benchmarks Game's 10 tasks, against its single-threaded C programs (`bench.py --suite game`). Each matches the Game's output file in both backends, in `make test`. regex-redux and pidigits use PCRE and GMP through foreign imports, as their C does. Geometric mean 1.16x C: binary-trees 0.4x, spectral-norm 0.8x, mandelbrot, regex-redux and pidigits 1.0x, fannkuch-redux 1.1x, n-body 1.2x, reverse-complement 2.1x. See [bench/README.md](bench/README.md#benchmarks-game-suite).

- Bitwise operations on `Int`, with `Data.Bits`' names and fixities: `.&.`, `.|.`, `xor`, `complement`, `shiftL`, `shiftR`. A shift by 64 or more gives 0 (or -1 for `shiftR` of a negative number), as GHC's; a negative shift is an error, so a shift by a variable is not treated as total.

- `isEOF ()`, for reading standard input line by line to its end; `getContents ()`, all of it at once, strictly; `indexString s i`, one character of a `String` without unpacking it; `writeArrayString arr i s`, a string's bytes into an `Array CChar` in one copy; `putArray arr i n` and `getArray arr i n`, a range of an array to standard output or from standard input; `copyArray src i dst j n`, as `memmove`.

- `IntTable` in the prelude: a mutable hash table from `Int` to `Int` in C memory, written in funk over `Array` (open addressing; doubles at three quarters full).

### Changed

- Reuse of matched cells extends to every data type, to the elements of a list nothing else references, to the components of a tuple result, and to any allocation of the same size, not only list cells at TRMC sites. A variable used at most once on each path owns its value, so a function may also return or store what it was passed. n-body allocates nothing per step: 2.8x to 1.2x C, 134 to 2 MB; `records` -22.5%. `bst`'s inserts now update in place, and its lookups are 27% slower on the resulting layout (+8.6% in all). See [docs/dev/element-reuse.md](docs/dev/element-reuse.md).

- A function whose every result is a tuple it builds returns it as a C struct in compiled code, so a `case` on its call allocates nothing; other uses box the tuple. Interleaved runs: n-body -30% (4.0x to 2.8x C), fasta -11%; the core suite -0.3%.

- `newArray` writes its first element and fills the rest by doubling copies (`copyArray`), and skips the fill when that element's bytes are zero, since the memory starts zeroed: an array's pages are then touched only when used. reverse-complement, reading in chunks into such a buffer, went from 0.49 to 0.25 s and 373 to 288 MB.

- Generated workers are `static inline`, except TRMC loops: the keyword raises clang's inlining threshold at `-O2`, so the prelude's small wrappers, such as `readArray`'s chain to `peek`, inline into loops. Interleaved runs: fannkuch-redux -53%, reverse-complement -31%, k-nucleotide -30%, records -9%; the core suite -0.8%, the game suite -14%. Inlined TRMC loops were up to 8% slower (sort's `randoms`), so they stay out of line.

- `readArray` and `writeArray` build their index error in a separate function, so the C compiler inlines them: spectral-norm -44.6% (now 0.8x C), fannkuch-redux -28%, k-nucleotide -26.5%. `newArray` fills with a loop instead of `mapM_` over `enumFromTo`, which built the list of indices first (300 MB for 12.5M elements). `getLine` reads a line with `getline(3)`, not a character at a time.

- A local binding whose right side calls a global function is evaluated where it is bound, not made a thunk, when that function's body is itself safe to evaluate early: `let m = max a b` at `Int`. A function that recurses, directly or not, or calls a parameter, still makes a thunk. `fannkuchredux` no longer allocates (135 to 2 MB resident); `fasta` -13.7%.

### Fixed

- A `data` constructor may hold a `foreign struct`'s type (`data St = St (Ptr Mpz)`); it was an unknown type, since data types were registered before structs. A `data` and a struct of the same name are still an error.

## [0.4.1] - 2026-10-27

### Changed

- funk is a static library, `libfunk.a`, with an internal api in `src/libfunk.h`, and a command line, `cli/main.c`, that uses only that api. The api returns errors and never exits. It is internal, with no stability promise, since the compiler's global tables allow one session per process.

- The test suite is one ctest test per check, so `make test` runs them in parallel (63 s to 21 s on 8 cores), and `ARGS="-R NAME"` or `ARGS="-L LABEL"` runs a subset. Each test has its own foreign-import cache and a 300 s timeout. `tests/check.sh` replaces `tests/run.sh`.

- Generated C defines nullary constructors (`[]`, `LT`, `Nothing`, a user's `Red`) as constant objects, not objects made at startup and read through the runtime's constructor table; clang sees their ids, and `sort`'s code does 5 fewer table loads. Interleaved runs: `lists` -9.6%, `sort` -5.4%, `wordfreq` -4.9%.

- `Int` boxing in generated C hints that a value fits in 63 bits (`__builtin_expect`): geometric mean -1.0% over the benchmarks, `sort` -5.3%. Of the 2.7% that 64-bit `Int` still cost, 0.7% remains; see [docs/dev/typed-backend.md](docs/dev/typed-backend.md#integers).

- Sweeping a chunk with nothing marked makes it a bump region instead of threading each dead object onto a free list. In `lists`, which drops every list each round, lazy sweeping fell from 209 to 7 ms of 950. Interleaved runs: `lists` -8.9%, `trees` -8.4%, `records` -6.3%, geometric mean -1.6%.

- A `case` in generated C tests `[]`, `:`, `Nothing`, `Just` and other constructors of types with at most two nullary ones by comparing addresses, not by loading the constructor id: nullary constructors are unique constants. A list loop's two tests become one compare, and clang drops the unreachable match failure. Interleaved runs: `sort` -12.3%, `lists` -11.1%, `wordfreq` -2.8%, geometric mean -2.0%.

- `show` at `Int` is its own primitive, `primShowInt` in C as `showi`, and typed code calls it on an `int64_t`: digits into a stack buffer, then one string, where it went through `snprintf`, a heap buffer filled byte by byte, and a copy. It allocates, so exports still reject it. Interleaved runs: `strings` -43%, `printing` -34%, `wordfreq` -6.8%.

- The C versions of `printing`, `strings` and `wordfreq` format numbers with the same digit loop as funk's `show` at `Int`, not `snprintf`: with funk's new `show`, they made funk read 0.6-0.9x C. The geometric mean is 1.19x C; their older C rows are not comparable.

### Fixed

- Generated C compiles without warnings under `-Wall -Wextra` (1,312 before, over the run tests, examples and benchmarks). `case` emits only the labels it jumps to; the program section ignores `-Wunused-variable` and `-Wunused-but-set-variable`, for temporaries a pattern does not read, which clang removes; warnings in foreign-import glue still show. The machine code is unchanged. A `wall` test per run test and example keeps it so.

- Incremental marking steps once per 256 KiB of allocation, not 1 MiB. Nothing is freed until a cycle ends, so a long marking window took fresh chunks: `bst` ran 6% slower than before 0.4.0's collector, with a 37% larger resident set. It is now 1.6% slower; the 0.4 ms pause bound is unchanged. Details in [docs/dev/realtime.md](docs/dev/realtime.md).

- `:l` with an unreadable file in the REPL reports it and continues; it ended the REPL.

## [0.4.0] - 2026-10-27

### Added

- `foreign export ccall "cname" name :: type` makes `cname` a C function that runs `name`, callable from any thread, for callers with deadlines such as audio callbacks. The compiler checks that the function's compiled code cannot allocate, and names what would: lists, strings, closures, lazy bindings, unknown calls. `Ptr`, `FunPtr` and `StablePtr` are now C addresses in typed code, and `castPtr`, `plusPtr`, `peek` and `poke` at scalar types compile to C, so pointer code no longer allocates. A literal at a known `Num` instance, such as `0 :: CInt`, calls its `fromInt` without a dictionary. A runtime error on a foreign thread aborts the program. See [docs/dev/realtime.md](docs/dev/realtime.md), R4.

- `droppedEvents ()`: queued callbacks that found the event ring full.

- The export check also rejects recursion other than a self call in tail position, a loop, so an export's stack use is bounded by its call graph.

- The export check also rejects code that can fail, since a runtime error on a foreign thread ends the program: `div`, `mod`, `quot` and `rem` by anything but a non-zero literal, `chr`, `floor` and the other partial primitives, and a `case` that does not cover every value. It accepts enumerations such as `Ordering`, whose values are static.

- `Array a`: a fixed-length array of `Storable` values in C memory, freed with `freeArray`. `arrayLength` acts (`->>`), since it reads memory `freeArray` frees. `readArray` and `writeArray` check the index; `readArrayWrap` and `writeArrayWrap` take it modulo the length and cannot fail, so a foreign export may use them. In typed code an array is a C address. C memory, over the funk heap, because a collected array could be freed while a foreign thread reads it.

- `Ring a`: a single-producer, single-consumer ring of `Storable` values in C memory, between any two threads, such as funk's thread and an audio callback. `ringPush` returns `False` when full; the consumer uses `ringCount`, `ringFront` and `ringDrop`, three calls where a `Maybe` would allocate.

- `sizeOf` in `Storable`, as in Haskell; `Storable` instances for `Array a` and `Ring a`.

- `truncateSat :: Float -> Int`: truncation that saturates at `Int`'s range and maps NaN to 0, so it cannot fail.

- `foreign struct ["header.h ctype"] T { field ["c.member"] :: type, ... }` declares a C struct: the header's, with its field types checked, or one laid out as C would. A field may be another foreign struct, held by value and reached with `fieldPtr`. `T.field :: Field T a` is a field's offset, from `offsetof`, and `T.size` the size; `readField`, `writeField` and `fieldPtr` use them, also in exports. C computes the layout, over funk, so it matches the platform's alignment rules.

- `atomicAdd` and `atomicCompareSwap` in `Atomic`, sequentially consistent; floats through a compare-and-swap loop.

- `Atomic`, with `atomicPeek` and `atomicPoke`: an acquire load and a release store, for memory shared between funk's thread and a foreign export. Instances for `Int`, `Float` and the C types.

- [examples/ffi/portaudio.fk](examples/ffi/portaudio.fk): a wavetable synth whose PortAudio callback is a foreign export, fed frequency changes through a `Ring`; 0 underflows in three runs, callbacks of at most 5 us against a 1.33 ms buffer, with four collection cycles during each run.

- `FUNK_GCSTATS=1` prints each collection cycle's pauses, and a summary at exit, in both backends. `FUNK_GCSTRESS=1` runs short collection cycles constantly and stops the program if a reachable object is left unmarked, for testing the collector.

- Foreign imports, phase 1 of [docs/dev/ffi.md](docs/dev/ffi.md): `foreign import ccall "math.h cbrt" cbrt :: Float -> Float` calls a C function, and `foreign library "name"` links `-lname`. Arguments and results are `Int`, `Float`, `Char`, `Bool`, `()` and the C types below; `->>` marks an import that acts. An import of a non-function type reads a C value once (`"math.h M_PI"`). Compiled code calls the C function directly, as fast as a primitive. The interpreter compiles the same glue with `$CC` into a shared library, cached by its hash in `$XDG_CACHE_HOME/funk/ffi`, and loads it: about 90 ms the first time, nothing after. Compiled glue, over libffi in the interpreter, lets imports call macros and `static inline` functions, and keeps both backends' conversions identical. `funk run` and `repl` take `-I`, `-L` and `-l` for the glue, and `build` takes `-I`.

- The declared types of an import are checked against the C declaration by the C compiler: a conversion that can change a value or drop a `const` is an error, so `abs :: Int -> Int` is rejected, since `abs` takes an `int`. Widening is allowed, and a `Ptr` passes as any object pointer. An exact comparison of the function type was tried first; it cannot pass `Ptr Db` where C has `sqlite3 *`.

- Foreign imports, phase 3: callbacks. `foreign import ccall "callback" f :: FunPtr (Ptr () -> a ->> r)` is the address of a C function that runs a funk closure, passed in its context argument as a `StablePtr`, on funk's thread; `"queue"` makes one that any thread may call, whose calls `waitEvent` and `pollEvent` run later. `CStringLen` in a callback's type is a C pointer and length, received as a copied `String`. An error in a callback is raised when the foreign call returns, never across C frames. A value import may be any C expression, such as `sizeof(T)` or `offsetof(T, f)`, for `peekByteOff` and `pokeByteOff`. Also: `allocaBytes`, `newCString`, `CBool`, `newStablePtr`, `deRefStablePtr`, `freeStablePtr` and the `FunPtr` and `StablePtr` casts. libremidi works ([examples/ffi/libremidi.fk](examples/ffi/libremidi.fk)). A trampoline per callback type, with the closure in the context, needs no code generated at run time, unlike Haskell's `"wrapper"`; C APIs without a context pointer cannot call funk.

- Foreign imports, phase 2: `String` arguments and results (`const char *`), `Ptr a`, opaque types (`data Db` without constructors), and Haskell's `Storable` (`peek`, `poke`), `alloca`, `mallocBytes`, `free`, `nullPtr`, `castPtr`, `plusPtr` and `peekCString`, with `CChar`. A NUL inside a `String` passed to C, or a NULL `String` result, is a run-time error rather than a shortened string. sqlite3 works in both backends.

- `CInt`, `CUInt`, `CLong`, `CULong`, `CLLong`, `CSize` and `CFloat`, C's scalar types, with `Eq`, `Ord`, `Show` and `Num` instances whose arithmetic wraps to the C width. `CULong` and `CSize` compare and show unsigned. A new class `Integral`, with `toInt`, and `fromIntegral :: (Integral a, Num b) => a -> b` convert between integer types; `toCFloat` and `fromCFloat` between `Float` and `CFloat`. These names are now reserved. Real types, over names that mean a range-checked `Int` in foreign signatures only, keep Haskell bindings close to their source and give C's wrapping arithmetic.

### Changed

- The glue cache rebuilds when a header the glue read is newer (from the compiler's `-MD` list), or a library named with `-l` in an `-L` directory has changed.

- A NULL `String` passed to a queued callback is an error raised after the event's slot is released, so the ring moves on.

- `fk_cb_check` in code an export runs on another thread leaves funk's thread's pending callback failure alone.

- A struct holding a header's struct by value includes that header; a failed REPL chunk's foreign structs are forgotten.

- A collection cycle's start pause no longer scans the stack: it makes the stack's older pages read-only, a write copies a page before it proceeds, and marking steps scan the pages. At a million frames the start pause fell from 28.9 ms to 0.2 to 0.27 ms and the longest pause is 0.47 ms. Protection over a `memcpy` of the stack, which a first version used: 0.2 ms against 9 to 12 ms for 96 MiB. The kernel cannot write into a read-only page (`EFAULT`), so a foreign call first makes the pages below its frame writable, and while a direct callback runs the stack is copied instead. Protection stops below the funk thread's first frame, above which glibc keeps thread data the kernel writes.

- `FUNK_GCSTRESS`'s check maps fresh pages over the dead stack at each cycle's start: unset slots in new frames held words of dead frames, which it reported as unmarked objects. `madvise(MADV_DONTNEED)` zeroes them on Linux but not on macOS. Its steps scan the stack's snapshot a chunk at a time, so a stressed run repeats.

- `plusPtr` adds as integers: C's pointer arithmetic on a null pointer, as in `plusPtr nullPtr 16`, is undefined.

- The collector sweeps lazily: allocation sweeps a chunk when its free list runs out, so a pause is roots and marking only. The longest pause of a program with 100,000 live cells fell from 17.9 ms to 1.0 ms, and with 1,000,000 from 35.0 ms to 9.8 ms; a sweep step costs at most 0.15 ms. Allocation-heavy benchmarks ran 2-8% faster. A chunk still unswept at the next collection has its marks cleared rather than being swept, which would put the sweep back in the pause. See [docs/dev/realtime.md](docs/dev/realtime.md).

- Queued callbacks go through a fixed ring of 4096 events: a trampoline claims a slot with a compare-and-swap and writes its arguments into it, where it called `malloc` and took a mutex. A full ring drops the event and counts it; bytes over 104 per event still use `malloc`. A callback on a real-time thread then waits on nothing funk holds.

- The collector marks incrementally: after a pause that scans the stack and the roots, it marks in steps of at most 0.4 ms, one per MiB allocated, and sweeping finishes in steps of the same size before the next cycle marks. The longest pause no longer grows with the heap: 0.40 ms with 1,000,000 or 4,000,000 live cells, where lazy sweeping alone left 9.8 and 37.5 ms. The start pause still grows with the stack, 29 us per 1,000 frames, and the heap is up to 70% larger. Three writes that overwrite a heap pointer, a thunk's update and a reused cell's head and tail, shade the old value while a cycle marks; compiled throughput changed by -0.2% to +7.9%, within what code placement alone moves. Snapshot at the beginning, over Dijkstra's insertion barrier, needs barriers only at those writes, since funk's other stores initialize new objects. See [docs/dev/realtime.md](docs/dev/realtime.md).

## [0.3.0] - 2026-10-26

### Changed

- Prelude functions and class methods cannot be redefined at top level: a definition of `lookup` or a method named `length` is an error, where it used to replace the prelude's. A name in a program then always means the prelude's, since without modules a shadowed prelude function could not be reached. Use another name or a type namespace (`Shape.lookup`); locals may still shadow prelude names. The prelude's types and constructors (`Maybe`, `Just`, `Ordering`) are reserved likewise.

- Effects are in types. A function arrow is `a -> b` if the function does not act by itself and `a ->> b` if it may; effects are inferred, and checked against signatures. A signature's `->` arrows share one effect, so `map :: (a -> b) -> [a] -> [b]` acts exactly when its argument does. Breaking: a signature that says `->` over a body that prints is an error; a top-level value that acts is an error, except `main` (make it a function); a `where` or `let ... in` binding that can act is an error, since its effect would run at a later use or under the wrong guard: bind it with `<-` or a `let` statement in `do`, where it runs on its line, or make it a function; a data field or class method written with `->` holds only pure functions. Chosen over Haskell's `IO` type because calls stay direct, with no runtime cost, and unannotated code needs no change. Design, prototype and alternatives: [docs/dev/typed-effects.md](docs/dev/typed-effects.md).

## [0.2.0] - 2026-10-25

### Changed

- Fresh heap chunks are allocated with a bump pointer instead of a free list built when the chunk is created, which wrote every object once before its use. The geometric mean over C improved 3%; `lists` 9%.

- Copies of higher-order functions made for a known function argument also take the call site's types, so `flip@{0=primMulInt}` works on C integers instead of boxed values. Copies are keyed by those types too, so `map id` at `[Int]` and at `[Char]` get separate copies. The geometric mean over C improved 5.7%.

- `Int` is 64 bits in both backends and wraps on overflow. Before, the interpreter wrapped at 63 bits while compiled code computed in 64 bits and truncated to 63 when an `Int` was stored in a list or passed to polymorphic code, so results after an overflow differed between backends and within compiled code. Values beyond 63 bits are boxed; integer literals and `readInt` accept the full 64-bit range. Chosen over 63-bit wrapping in compiled code, which measured 29% slower against 5.4% for boxing. The geometric mean over C went from 1.37x to 1.48x.

- Constructor fields declared `Float` are stored unboxed in compiled code: `data Vec = Vec Float Float` takes 3 words instead of 3 words plus two 2-word boxes, and reading a field does not follow a pointer. A program building 2M such values runs 2.8x faster. The interpreter keeps boxed fields.

- Fixed: a negative-zero literal, `(-0.0)`, compiled to `0.0`. The code generator printed Float literals with `%g`, which renders `-0.0` as the integer `-0`; it now emits hex float literals, which are exact.

- Closures and thunks store captured `Int`, `Float`, `Char` and `Bool` values raw instead of boxed, so capturing a `Float` no longer allocates. The benchmarks do not change; a program building 2M closures over `Float`s runs 8% faster.

- Compiled functions take and return C scalars: each is a worker with typed parameters, called directly, and a uniform wrapper for closures and unknown calls. clang can now inline small functions such as `even` and `compare`. The geometric mean over C went from 1.52x to 1.37x; `fib` is at C speed. One effect: clang turns some non-tail recursions, such as `1 + f (n - 1)`, into loops, so compiled code may run them in constant stack where the interpreter overflows.

- Compiled and interpreted code fuse chains of prelude list functions into one loop, without the intermediate lists. Chains are built from `enumFromTo`, `replicate` or any list, through `map`, `filter`, `take`, `takeWhile` and `concatMap`, into `foldl`, `sum`, `length`, `elem`, `any` or `all`, or back into a list. A chain is fused only when the result, the error raised and termination are unchanged: no stage performs an effect, at most one stage can fail, and no stage before `take` or another short circuit can fail. Exact semantics were chosen over Haskell-style imprecise errors, which would fuse more chains but make error messages depend on the optimizer. `FK_NOFUSE=1` turns fusion off. The benchmark `lists` is split: `pipeline` is the old program, compared with a fused C loop, and `lists` names its lists so they stay. The geometric mean over C went from 1.68x to 1.52x; `queens` from 2.9x to 1.4x. Design: [docs/dev/fusion.md](docs/dev/fusion.md).

- Compiled code writes the output of `map`, `filter`, `take`, `zip` and similar functions into the cells of their input list, when that list is fresh: the direct result of a call that builds new cells, such as `filter even (enumFromTo 1 n)`. A static check proves that nothing else references the input; a variable never counts as fresh. Chosen over Perceus reference counting, which a prototype showed slower: freeing cells as they die scatters later allocations. Separately, an alternative that rebuilds the cell it matched (`merge`'s `x : xs`) returns that cell. The geometric mean over C went from 1.79x to 1.68x; `sort` is 30% faster. Design: [docs/dev/reuse.md](docs/dev/reuse.md).

- `String` is a packed builtin type, no longer `[Char]`. `++` and `length` are class methods with instances for lists and `String`; `words`, `lines`, `unwords`, `unlines` and the new `concatStrings` are primitives. List functions need `unpack :: String -> [Char]`, and `pack` converts back. A string literal pattern compares by equality. Chosen over keeping `[Char]` for speed: one 3-word cell per character becomes one byte. `strings` runs at 1.6x a packed C version; it was 3.3x a `[Char]` C version.

- Generated code is faster: on the benchmark suite, its geometric mean over C went from 10.3x to 1.35x (`docs/dev/performance-first.md`). Local bindings that cannot fail, loop or act are evaluated where they are bound, without a thunk. Calls with closed function arguments, such as `map (* 3) xs`, call copies of the callee in which the argument is a known function. Functions that build a constructor around a call to themselves, such as `map`, `filter` and `++`, compile to loops (tail recursion modulo cons), so a long `map` no longer uses stack proportional to its length in compiled code. Local functions that are only called become top-level functions taking their free variables, and lambdas that capture locals are specialized like closed ones. Values of type `Int`, `Float`, `Char` and `Bool` are C scalars inside function bodies (`docs/dev/typed-backend.md`). Typed primitives are inlined, comparisons in conditions compile to C comparisons, stack overflow is detected by a guard region instead of a check in every function, and only reachable code is emitted. The collector finds objects by address arithmetic instead of binary search, skips non-pointers, no longer zeroes dead objects, and collects after max(128 MiB, 2x live) instead of max(32 MiB, live), which roughly doubles peak memory.

- `Float` comparison follows IEEE in both backends: `NaN == NaN` is `False`. It was `True`, because floats were compared structurally.

- `.` with no space on either side is field access: `f.g` is no longer composition.

- Non-recursive `let` and `where` value bindings are evaluated on first use, at most once. Guards can now protect a `where` binding: `f x | x == 0 = 0 | otherwise = y where y = div 1 x`. A thunk is chosen over strictness analysis for size. It costs one allocation per evaluated binding. Effects bound by `let` run at first use.

- `Eq`, `Ord`, `Show` and `Num` are type classes, replacing the `Num` flag and structural `==`/`show`. Comparing or printing a data type requires `deriving (Eq, Ord, Show)`; without it, `A == B` is a type error. `show` follows the type, so `print (one () :: Float)` prints `1.0` and `print ""` prints `""`. A constrained type nothing fixes is an error, as in Haskell: `print []`. Local functions without a signature no longer generalize constrained type variables, so specializing a function also specializes its `where` loops; one used at two number types needs a signature.

- Literals in overloaded code are built through `fromInt`, so a polymorphic literal used as a `Float` is a `Float`. 0.1 built it as an `Int`.

- `infix`, `infixl`, `infixr`, `class`, `instance` and `do` are reserved words, and `<-` is a reserved operator.

### Added

- A CMake build (`CMakeLists.txt`), with `make` as a frontend: it configures on first use, builds in `build/` and links `./funk`. `make SANITIZE=undefined test` builds and tests funk under a sanitizer in its own directory, and `make install` installs it. `ctest` runs the test suite.

- Command line: `funk check FILE` type-checks without running; `funk eval EXPR` (`-e`) evaluates an expression; `funk repl FILE` loads a file into the REPL. `funk build` takes `-O LEVEL`, `-l LIB` and `-L DIR`. `funk help [COMMAND]`, `-h` and `--version` print to standard output. Usage errors name the problem and the help to read, and exit with status 2; failing programs exit with 1.

- `do` blocks with `p <- e`, expression and `let` statements. Each statement becomes `case e of p -> rest`, chosen over `>>` and over a strict bind primitive because both evaluate the rest inside a call: a recursive `do` loop of 20M iterations overflowed the 4 GiB stack, and now runs in constant stack. Without an `IO` type, `<-` accepts any expression. A `where` at the column of a block's items now closes that block.

- Type classes, phase 1 (`docs/dev/type-classes.md`): single-parameter `class` and `instance` declarations with superclasses, default methods and instance contexts, and `deriving (Eq, Ord, Show)`. Constraints are passed as dictionaries, chosen over monomorphization because the REPL interpreter and polymorphic recursion need code that works at any type. Calls whose dictionaries are all known call a specialized clone, `fib@Int`, so monomorphic code runs as fast as in 0.1.

```haskell
class Shape a where
  area :: a -> Float
  describe :: a -> String
  describe x = "area " ++ show (area x)
```

- Records with fields scoped to their type: `data P = P { x, y :: Float }`, construction `P { x = 1.0, y = 2.0 }`, update `p { x = 3.0 }`, patterns `P { x = a }`, and dot access `p.x`, `(.x)`. `Type.f` defines a function in a type's namespace, and `p.f` calls it with `p` first. Type-scoped fields were chosen over Haskell 98's global selectors so that two types can share a field name. The cost is that `p.x` needs `p`'s type: `getX p = p.x` needs a signature. See `docs/dev/records.md`.

- Benchmark suite (`make bench`): 10 programs in funk and in C that use the same algorithm and data structures. Each run appends timings, max RSS and build times to `bench/results.jsonl`, keyed by host and commit. `bench/bench.py report` flags regressions against an earlier run.

- `infixl`, `infixr` and `infix` declarations at top level, as in Haskell 2010. Declarations are installed by pre-scanning the module, chosen over re-associating parsed operator chains because it leaves the precedence-climbing parser unchanged. The cost is no local fixity declarations.

- Compile-time warning for a `case` or function that does not cover every constructor, naming an unmatched value. Guarded alternatives count only when a guard is `otherwise` or `True`.

### Fixed

- Type errors are reported at the sub-expression with the wrong type, not at the binding. `f :: Int -> Bool; f x = x + 1` reported `expected Int -> Bool, found Int -> Int` at the start of `f`, and now reports `expected Bool, found Int` at `x + 1`. Expected types are now pushed into lambdas, `let`, `if`, `case` and constructor arguments, so a signature's types are known before the body is inferred. Before, `m :: Int -> String; m n = case n of 0 -> "z"; _ -> n` reported `no instance Num [Char]`, because `n` had already been unified with `String`. Rigid type variables print with their names from the signature.

- Tuple constructors were registered with no type (`tydef` -1), because the type name was looked up without interning. The C backend read `tydefs[-1]` for every tuple pattern.

## [0.1.0]

### Added

- Language with Haskell syntax: clauses with pattern matching and guards, `where`, `let`, `case`, `if`, lambdas, layout rule, `data`, sections, ranges, operator definitions, `.`, `|>`, `>>>` and `$`.

- Hindley-Milner inference with optional, rigid signatures. Numbers use a single `Num a` constraint over `Int` and `Float` with polymorphic literals, chosen over type classes for size; see `docs/dev/design.md`.

- Strict evaluation with effects sequenced by `>>`.

- REPL (`funk`) and interpreter (`funk FILE`).

- Compiler to standalone C (`funk build`, `funk emit-c`).

- Shared runtime with a conservative mark-sweep GC and a 4 GiB lazily committed stack.

- Prelude of 78 functions over lists, `Maybe`, `Either`, characters and strings.

- Test suite (`make test`): every run test is checked under both backends.
