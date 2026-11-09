# Patch port

0.11's acceptance test ([roadmap.md](roadmap.md#011----tidal-expressive-patterns)): real Tidal patches ported to funk, with each difference listed, and the slowest query timed in the interpreter. Done 2026-11-08.

## Source

The 22 patches of `old/examples/cycles.tidal` in Tidal's repository ([tidalcycles/Tidal](https://github.com/tidalcycles/Tidal/blob/fefb1c3/old/examples/cycles.tidal), fefb1c3). The file's header says they were posted to @tidalcycles on Twitter. Each combines 3 to 8 functions, such as `jux`, `spread'`, `striate`, `weave`, `smash` and `whenmod`.

The alternative was the one-line examples in tidal-core's comments. They show one function each, which `lib/tidal` already covers ([combinators.md](combinators.md#status)).

## In Tidal 1.10.2

The patches use Tidal's names from before 1.0. `tests/tidal/Patches.hs` has them in tidal-core 1.10.2's. The old meanings come from tidal 0.2.11 and 0.4.33 on Hackage.

- **`|+|` is `#`.** In 0.2.11 and 0.4.33, `|+|` is `merge`, a union of the two maps with structure from the left (`Stream.hs:164-169` in 0.2.11). 1.10's `#` is that union. On a key both sides have, 0.2.11 keeps the left's value, and 0.4.33 and `#` the right's. Only `p1` has such a key: `speed`.
- **`echo t` is `stutter 2 t`**, as 0.2.11 defines it (`Strategies.hs:16`). 1.10's `echo` takes a count, a time and a feedback.
- **Signals.** `sinewave1` and `sine1` are `sine`, and `tri1` is `tri`. `rattri1` is in neither 0.2.11 nor 0.4.33. It is read as a rational `tri1`, as `sinerat1` is a rational `sine1` (inference).
- **`spread' f` is `spread' (f . pure)`.** 0.2.11's `spread'` takes a function of a value. In 1.10, `striate`, `slow`, `<~` and `density` take a pattern.
- **`smash 4 [1..16]` is `smash 4 (map pure [1..16])`.** 1.10's `smash` takes patterns.
- **Mini-notation.** Tidal 1.10.2 rejects a step with two modifiers when more of the sequence follows: `"a*2/5 b"` and `"a/5*6 b"` fail with `unexpected ' '`. At the end of a string it parses them, but `"a b*1/3"` plays `a` from 0 to 1.5 (observed). 11 places in 8 patches are rewritten:
  - `x*1/n` is `x/n`, in `p0`, `p2`, `p3` and `p4`;
  - `x*a/b` is `[x*a]/b`, in `p5`;
  - `x/a*b` is `[x/a]*b`, in `p7`, `p7.1` and `p16`.

  funk's parser accepts all three forms.

## In funk

`tests/tidal/ports.fk` has the port, and [examples/tidal](../../examples/tidal/README.md) has 18 of its patches to play. Beyond the `p.` names:

- **String literals are patterns**, as with Tidal's `OverloadedStrings`, since 2026-11-09, and controls take patterns, so `sound (samples "bd*2" (run 8))` and `end (slow 5 sine)` port as written. Until then the port used `p.miniS`, `p.mini` and `pF "end"`, and defined `sound` of a pattern as `soundP`. One literal keeps `p.miniS`: `p0`'s `let x = ...`, which nothing else fixes to a pattern ([tidal-comparison.md](tidal-comparison.md) L1).
- **`pure` is `p.atom`**, and `<$>` and `fmap` are `p.mapP`. Sections and `.` port as written: `(# vowel "a") . p.iter 4`.
- **Speed factors are `Int`s.** `slow (3/5)` is `p._fastR 5 3` (`p11`). `spread' (density . pure)` over a continuous `Float` is `p.density . p.atom . round` (`p14`). That channel matches only because it has no onsets in either (below).
- **Times are `Float`s.** `toRational <$>` is dropped. `(26/8) <~` ports as written, since `Pattern` is `Fractional`.

## Results

`lib/tidal-patches` (`tests/tidal/patches.sh`) queries each patch's onsets for 8 cycles, 2 for `p6.1`, in both. `tests/tidal/match.py` matches the events. A match is exact, or within 32 ticks and 1e-3 in each value (rounding, below). `tests/tidal/patches.expected` holds the counts.

| | events | exact | rounded |
|-|-|-|-|
| `p0` | 31 | 0 | 31 |
| `p1` | 380 | 32 | 348 |
| `p2` | 479 | 331 | 148 |
| `p3` | 179 | 179 | 0 |
| `p4` | 88 | 70 | 18 |
| `p5` | 345 | 345 | 0 |
| `p6`, `p6.1`, `p6.2` | 0 | 0 | 0 |
| `p7` | 687 | 173 | 514 |
| `p7.1` | 346 | 127 | 219 |
| `p8` | 219 | 189 | 30 |
| `p9` | 1418 | 1418 | 0 |
| `p10` | 9 | 9 | 0 |
| `p11` | 1518 | 1518 | 0 |
| `p12` | 32 | 32 | 0 |
| `p12.1` | 42 | 42 | 0 |
| `p13` | 148 | 69 | 79 |
| `p14` | 40 | 12 | 28 |
| `p15` | 0 | 0 | 0 |
| `p16` | 189 | 0 | 189 |
| `p17` | 306 | 306 | 0 |
| total | 6456 | 4852 | 1604 |

No event is unmatched.

- **Silent in Tidal 1.10.2:** `p6`, `p6.1`, `p6.2` and `p15`, and one channel of `p12`'s `jux`. Each uses `spread'` over a continuous pattern. 1.10's `spread'` is `>>=`, whose whole is the intersection of both wholes, and a continuous event has none (`unwrap`, `Pattern.hs`). funk does the same. In 0.2.11, `spread'` applied its function at each onset of the outer pattern, so these sounded when written (inference, from `Strategies.hs:44-47`).
- **`p10`** has `bass3/0`, which is silence in both.
- **One bug, fixed:** the wholes of a mini-notation step longer than a cycle ([CHANGELOG.md](../../CHANGELOG.md)). 85 events of `p1`, `p3`, `p5`, `p7` and `p7.1` differed. `lib/tidal` has a case for it: `miniStep`.

### Rounding

1604 events round, by at most 30 ticks in time and 4e-6 in a value. 30 ticks is 42 us at 0.5 cycles per second.

- 720720 is 2^4 * 3^2 * 5 * 7 * 11 * 13. A cycle divides exactly into 16ths, but not into 32nds.
- `p16`'s `weave 16` runs an 8-step sequence 16 times as fast, in 128ths. `_fast` rounds down, and an outer `slow` multiplies the error.
- A value sampled from a continuous pattern, such as `p4`'s `speed` from `tri`, is taken at the rounded time.

With 46126080 ticks per cycle (720720 * 64, so 2^10), every event outside `p17` is exact. `p17`'s `rand` then differs, as `randAt`'s constants assume 720720 (scratch run, 2026-11-08; not kept).

## Timing

`tests/tidal/bench.fk` queries each patch in 10 ms windows at 0.5 cycles per second, as the plugin does (`kCycleMs` 2000, `kWindowMs` 10). It times each window with `clock()`. Interpreter (`funk run`), Apple M1, 3 runs, load average 2.6, 2026-11-08.

| | slowest window, us | mean, us |
|-|-|-|
| `p11` | 1515 - 2044 | 800 |
| `p5` | 1265 - 1268 | 605 |
| `p2` | 705 - 804 | 249 |
| `p8` | 770 - 823 | 378 |
| `p13` | 649 - 750 | 295 |
| `p15` | 696 - 716 | 346 |
| `p6.1` | 421 - 1151 | 323 |
| the other 15 | 139 - 578 | 51 - 197 |

- The slowest window took 2.0 ms of its 10 ms, in one run. Every mean is at most 0.8 ms, 8% of the window.
- Compiled (`funk build`, 1 run), the slowest window took 352 us and every mean was at most 30 us.
- Only the query is timed. The hosts also write each event's record: at most 8 events in any window here.

The roadmap does not define "well inside". At 20% of the window, with a 50 ms lookahead in `funk-midi`, this passes (judgement).

## Open

- **The tick rate.** 46126080 makes binary subdivisions down to 1024ths exact. The cost: `randAt`'s constants, `funk-midi`'s `CYCLE_TICKS`, and every expected output written in ticks. A tick count would reach Int64's limit at 2e11 cycles, less by any `_fast` factor. The alternative is to keep 720720, with errors of at most 30 ticks.
- ~~**Tidal's `sound` of a pattern.** 12 of 22 patches need it.~~ Done 2026-11-09: controls take patterns.
- **Fractional patterned speed factors**, for `p11` and `p14`.
