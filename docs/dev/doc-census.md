# Doc-example census

2026-11-10. The `d1 $` examples in tidal-core 1.10.2's and tidal 1.10.3's doc comments, typed into funk-midi unchanged, to rank the remaining 0.12 gaps of [tidal-comparison.md](tidal-comparison.md). It follows [patch-census.md](patch-census.md), whose 2014 patches could not rank them.

## Method

- 264 examples, each joined into one line: UI.hs 147, Core.hs 44, Control.hs 40, Params.hs 18, Pattern.hs 9, Transition.hs 4, Boot.hs 3, Scales.hs 1.
- Tidal side: each example through `tests/tidal/Dirt.hs`'s `out` in ghci, as `d1` plays it, with `Data.Ratio` in scope. That is 800 windows of 10 ms at 0.5 cps, encoded by Tidal's `toOSC`.
- funk side: `funk-midi --no-sleep --cycle 2000 --dirt`, as `tests/tidal/dirt.sh` runs, with the example as its only input.
- An example matches exactly when the two sides send the same `/dirt/play` messages, bit for bit. Otherwise it matches by rounding when every message pairs with one within 36 ticks and 1e-4 in each value.
- ctest `lib/tidal-docs` (`tests/tidal/docs.sh`) reruns this. `tests/tidal/docs.cases` holds the 248 examples Tidal runs, and `tests/tidal/docs.expected` holds each one's result.

## Results

| | Examples |
|-|-|
| Tidal rejects | 16 |
| Exact | 185 |
| Rounded, at most 16 ticks (44 us at 0.5 cps) | 35 |
| Rounded, kept: `# cps` sets the tempo (C2) | 6 |
| Differ, kept: `stripe` (F9) | 1 |
| funk rejects | 21 |

So 227 of the 248 that Tidal runs run unchanged in funk, and 226 send what Tidal sends.

- **Tidal rejects 16.** 9 need definitions the doc only describes (`pats`, `fx`, `myscale`, `sine1`), or `step'`, which Tidal doesn't export. The others:
  - `slow (4%3)` and `spread slow [2,4%3]`: `Time` is not `Ratio Integer`.
  - 3 have `%` in mini-notation for a `Pattern Time`.
  - `'bite'` has quotes left over from Haddock markup.
  - an `up` example has an unbalanced `]`.
  - `accelerateTake` raises `toDatum: unhandled value` when played.
- **Rounded 35.** Where traced, each divides a cycle finer than 1/16: `chop 32`, `*32`, `striate 128`, or inside a function, as `weave 16` does. 720720 ticks hold 2^4 but not 2^5. funk-midi plays onsets, so 44 us is inaudible. T1's finer ticks remain unneeded.
- **`# cps`, 6.** funk-midi applies an event's `cps` to the tempo, as `Stream/Process.hs:151` does. `Dirt.hs` holds 0.5 cps. Measured in cycles, the two sides match by rounding, except the `delta` of the onsets at cycle 0. funk-midi times those at the old tempo, as Tidal does: `processCps` reads `on`, `off` and the tempo from the frame's session state and sets the new tempo in another (`Stream/Process.hs:136-163`). The two differ only in that span: Tidal's frame is 50 ms (`clockFrameTimespan`), funk-midi's window 10 ms. So `# cps` stays kept (C2) for `Dirt.hs`'s fixed tempo, not for funk-midi.
- **`stripe`, 1.** Tidal's `substruct'` returns events outside the query, so its 800 windows send 3220 messages for 48 onsets. funk sends each of the 48 once.

## funk rejects 21

| Blocker | Item | Count |
|-|-|-|
| `Bool` mini-notation: `struct "t ~ t*2 ~"`, `mask "1 0 1 1"`, `stitch "t(7,16)"` | M1 | 9 |
| `grain'`, `drum` | C3 | 2 |
| `silent`; a string as a `ControlPattern` (`Simple.hs:31`, `"realclaps" # gain 0.8`) | F4 | 2 |
| `quantise` returns only `Float`, `chromaticiseBy` only `Int`; Tidal's are polymorphic, so `n (quantise 1 ...)` fails | new | 2 |
| `scale` | F1 | 1 |
| `<>` on patterns | new | 1 |
| `show` gives a `String`, which is not `[Char]` (`when (elem '4' . show)`) | new | 1 |
| a qualified module path, `Data.Fixed.mod'` | new | 1 |
| `Map.singleton` and `VN` | new | 1 |
| `\|>` is the prelude's pipe | L6 | 1 |

The census confirms L6: 1 example uses `|>`, and 108 use `#`.

## After M1

2026-11-11: 8 of the 9 examples M1 blocked match exactly, so 193 are exact. The ninth, `ccv (stitch "t(7,16)" 127 0) # ccn 0 # "midi"`, now stops at F4's string as a `ControlPattern`.

## After F4 and C3

2026-11-11: F4's 12 functions and its string as a `ControlPattern`, C3's `grain'`, `midinote`, `drum` and `drumN`, and a polymorphic `chromaticiseBy`. 199 examples are exact and 7 rejected:

- `<>` on patterns;
- `show` gives a `String`, not a `[Char]`;
- `Data.Fixed.mod'`;
- `|>` (L6);
- `Map.singleton` and `VN`;
- `quantise` on notes (L9);
- `scale` (F1).

## After L9 and F1

2026-11-11: 201 examples are exact and 5 rejected. `quantise` on notes and `scale` now match Tidal exactly.

## After L10

2026-11-11: `<>` on patterns, so 202 examples are exact and 4 rejected.

2026-11-11: 46126080 ticks per cycle (T1), so 235 examples are exact and 2 rounded, by at most 23 ticks (1 us at 0.5 cps). `chop 256` over `[hh future]*2` needs 2^11 steps a cycle; `Params:3394` sets `cps` in the pattern, and its times round to microseconds.

## Limits

Doc comments give each function about one example. So the counts measure how many functions a gap blocks, not how often live code calls them. F1 blocks 1 example here, but `n (scale "major" "0 .. 7")` is common in sets (judgement, not counted). M1 blocks 9 examples of `struct`, `mask` and `stitch`; `sew`, `while` and `euclidOffBool`'s users need it too.
