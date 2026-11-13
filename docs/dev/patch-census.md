# Patch census

2026-11-10. The 22 patches of [patch-port.md](patch-port.md), in their Tidal 1.10 form (`tests/tidal/Patches.hs`), run through funk unchanged, to rank the remaining 0.12 gaps by what real code hits. Each blocker found was shimmed in a scratch copy to reach the next one.

## Method

- Each patch is one file: Tidal's line as `pat`, under `import pattern (..)` and `control`'s names but `clip`. This assumes L5, an unqualified `pattern`, is done.

- `funk check`, then `funk run`, printing as `tests/tidal/patches.fk` does. `tests/tidal/match.py` matches the events against Tidal's.

## Blockers

| Blocker | Item | Patches | Count |
|-|-|-|-|
| `pattern` is imported as `p` in the hosts | L5 | all | 22 |
| `clip` is in both `pattern` and `control` | L5 | all, under an unqualified import | 22 |
| `pure`, `<$>` and `fmap` on patterns | L3 | p1, p3 to p9 with p6.1, p6.2 and p7.1, p12, p12.1, p13 to p15 | 16 |
| `toRational` | T3 | p12, p12.1, p13 | 3 |
| a fractional speed factor: `slow (3/5)`, `density` of a `Float` pattern | T2 | p11, p14 | 2 |

None of the patches needs T1's finer ticks: rounding stays at most 30 ticks, as in the port.

## Results

- Unchanged, with Tidal's names in scope: 5 of 22 type-check (p0, p2, p10, p16, p17).

- With L3 and `toRational` shimmed: 20 of 22 run. Their counts of exact and rounded matches equal `tests/tidal/patches.expected`'s, so no new difference appears.

- p11 and p14 fail on T2 alone.

## After L5 and L3

2026-11-10, with the hosts' imports, `pattern (..)` and `control (..)`: 17 of 22 run unchanged, and 20 with `toRational` stubbed, all matching as `tests/tidal/patches.expected` does. p11 and p14 wait for T2.

After T2: 19 of 22 run unchanged, and all 22 with `toRational` stubbed as `Float -> Float`, matching as `tests/tidal/patches.expected` does.

## Limits

The patches date from about 2014. They use `striate`, `spread'` and `weave` heavily and none of `n "0 .. 7"`, scales, `struct "t(3,8)"`, `arp`, `chop` or `sometimesBy`. So they cannot rank F1-F8 or M1. tidal-core 1.10.2's and tidal 1.10.3's doc comments hold 253 `d1 $` examples in the current API: `n` 51 times, `every` 18, `chop` 12, `struct`, `mask` and `sew` 11. [doc-census.md](doc-census.md) runs them.
