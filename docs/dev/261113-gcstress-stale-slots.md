# FUNK_GCSTRESS: an unset spill slot read as a root

2026-11-13. On Linux with GCC 13.3 and 15.2, x86-64, `hosts/slots-gcstress` and `hosts/slots-midi-gcstress` failed the check of marks: "reachable object ... (kind 2) is unmarked at the end of a cycle, in the stack ... in a frame from before the cycle's start". Both tests were added with the fix in [261110-gcstress-chkstk.md](261110-gcstress-chkstk.md) and had not passed on Linux. macOS, and clang on x86-64 under Rosetta, passed.

## Cause

The collector was correct. The check read a slot the program never set.

1. `fk_apply` has two paths that call. A closure of matching arity: `return FK_CODE(f)(f, args)`. A partial application: copy into a VLA, then call `fk_apply` again.

2. GCC gives the partial-application path spill slots in the fixed frame: `-0x48`, `-0x50` and `-0x58(%rbp)`, around its `memset` and `memcpy`. The closure path never writes them.

3. The reported slot was `-0x50(%rbp)` of an `fk_apply` frame in the closure path; its return address on the stack was the instruction after `call *0x8(%rbx)`. It held a word from an earlier call made during the cycle, a pointer to an object dead since the cycle's start.

The slot was above the snapshot's stack pointer, so the dead-stack clearing at the cycle's start did not reach it.

## Evidence

- In an Ubuntu 24.04 amd64 container (GCC 13.3, as CI), `slots_midi` failed 5 runs of 5 with CI's addresses.
- With every unmarked object's payload set to `0x10` before the sweep, the check off, both scripts ran unchanged: 536,018 and 913,747 objects poisoned.
- The frame and slot came from `_Unwind_Backtrace` and a dump of the words around the slot at the failure, against `objdump` of the binary. `_Unwind_GetCFA` paired with a frame's IP gave its lower bound, not its CFA; the return address on the stack settled which frame held the slot.

## Fix

The check records each object's state at a cycle's start: free, allocated and unreachable, or reachable. Its roots are the collector's: the stack's snapshot, the snapshot's read-only pages, still unwritten in the start pause, and the root tables. At the cycle's end, a stack word to an unmarked object that was unreachable at the start is skipped: a program cannot obtain a pointer to an unreachable object, so the word is stale.

Each source of stale words had been removed in turn: the dead stack (0.4.0, fresh pages), `eval`'s VLA padding under ASan (0.8.0, ASan's VLA checks off), clang's `___chkstk_darwin` (0.11.0, the VLAs zeroed). Spill slots are laid out by the compiler, so a fifth removal would not have been the last.

The 0.11.0 zeroing is gone: the 4 VLAs are as before it. On macOS without it, the slot tests pass 10 runs of 10. With the new skip disabled as well, both fail again, each on a `K_THUNK` "unreachable at the cycle's start": stale stack words still occur on macOS, presumably `___chkstk_darwin`'s (not traced), and the check now skips them.

The check still fails for an unmarked object that was reachable at the cycle's start, as a missing deletion barrier leaves, or that was allocated during it, as a missing black allocation leaves. With `alloc_black` disabled, `tests/run/gcbarrier.fk` and `slots_midi` failed "allocated during the cycle"; with `fk_shade` disabled, `gcbarrier.fk` failed "reachable at the cycle's start".

What it no longer catches: a pointer the start pause could not see, such as one held only in C memory that is not a root, which reappears on the stack before the cycle ends. Registers, root tables and heap fields stay strict.

## Reproducing on Linux from macOS

Apple's `container` runs an amd64 Ubuntu under Rosetta, which matches CI's compiler:

```sh
container system start
container run -d --name funk-x86 --arch amd64 --rosetta -c 4 -m 8G \
  --mount type=bind,source=$PWD,target=/src,readonly ubuntu:24.04 sleep infinity
container exec funk-x86 sh -c 'apt-get update && apt-get install -y build-essential cmake ninja-build python3 libsqlite3-dev'
container exec funk-x86 sh -c 'mkdir /w && cd /src && tar --exclude=./build --exclude=./.git -cf - . | tar -xf - -C /w'
container exec funk-x86 sh -c 'cmake -S /w -B /b -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo && cmake --build /b'
```

Build from a copy: run from the bind mount, `kernels/ugens` segfaults, on HEAD too; the cause is not known. In this container, CI's 24.04 passes but these fail:

- the `tsan` and `/thread` tests: ThreadSanitizer cannot map its shadow memory under Rosetta;
- `embed/kernels`: its missing compiler's message differs.

gdb cannot trace under Rosetta (`PTRACE_GETREGS` fails); a native arm64 container (`--arch arm64`, no `--rosetta`) runs it. The 26.04 image's `tar` fails under Rosetta ("Function not implemented"); `cp -r` works.
