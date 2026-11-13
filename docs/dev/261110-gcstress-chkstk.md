# FUNK_GCSTRESS: a scratch register read as a root

2026-11-10. Under `FUNK_GCSTRESS=1`, funk-midi failed the check of marks: "reachable object ... is unmarked at the end of a cycle, in the stack ... in a frame from before the cycle's start". HEAD cbd9992 failed too, given a module `data ID = ID String deriving (Eq, Ord, Show)` and one more definition. R1 made every slot script fail.

## Cause

The collector was correct. The check read a word that the program never used as a value.

1. `fill_closure`, inlined into `eval`, stores free variables through a post-incremented pointer (`str x12, [x11], #8`). After the loop, x11 points one past the closure, at the next object in its chunk.

2. `eval` then declares `V args[n]`. Clang calls `___chkstk_darwin` first, which saves x10 and x11 at `[sp, #-16]`: the top of the block the VLA then takes.

3. That word sits in an unset slot or in the VLA's padding. The check scans it as a root and finds the next object, dead since the last cycle and so unmarked.

The words never reach marking's snapshot, so they cause no retention outside the check.

## Evidence

- With every unmarked object's payload poisoned at the end of each cycle, before any sweep, the whole suite ran unchanged under `FUNK_GCSTRESS=1`, the check off. A freed object in use would have crashed.

- At the start of the failing cycle, no stack word, register, root, heap word or other writable memory pointed to the object, except a stale mark-stack entry.

- An lldb watchpoint on the reported slot stopped in `___chkstk_darwin`, with x11 equal to the object's address. The object below it was a 4-word interpreted closure.

## Fix

The 4 VLAs in `interp.c` and `rt.c` have an even length, so they fill their 16-byte block, and start zeroed under `FUNK_GCSTRESS` (`FK_STRESS_ZERO`, `rt.h`). This matches the dead stack's clearing in `snapshot_stack`. Clearing the scratch registers at a cycle's start did not help: the pointer is made during the cycle.

Tests: `hosts/slots-gcstress` and `hosts/slots-midi-gcstress` fail without the zeroing. `bench.py ab HEAD`: +0.1% geometric mean.
