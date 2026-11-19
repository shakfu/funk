# Tidal patches

18 of the 22 patches of Tidal's [old/examples/cycles.tidal](https://github.com/tidalcycles/Tidal/blob/fefb1c3/old/examples/cycles.tidal), ported to funk. Each file has its original in a comment. [docs/dev/patch-port.md](../../docs/dev/patch-port.md) lists the edits and checks every event against Tidal 1.10.2. `# 6`, `# 6.1`, `# 6.2` and `# 15` are left out: they are silent in Tidal 1.10.2.

## Playing

The kits read Dirt-Samples from `~/.local/share/funk/Dirt-Samples`. funk does not ship it: it has no license file, and it is about 400 MB. Fetch it at the commit the examples were checked with:

```sh
scripts/dirt-samples.py
```

If SuperDirt already installed a copy, link it instead:

```sh
mkdir -p ~/.local/share/funk
ln -s "$HOME/Library/Application Support/SuperCollider/downloaded-quarks/Dirt-Samples" ~/.local/share/funk/Dirt-Samples
```

That is macOS's path; on Linux it is `~/.local/share/SuperCollider/downloaded-quarks/Dirt-Samples`. Then load a file in the plugin, or run it in `funk-midi`:

```sh
funk-midi examples/tidal/cycles-05.fk --audio
```

`--dirt 57120` in place of `--audio` sends the events to a running SuperDirt instead.

`funk-midi` plays in real time only when built with libremidi: `cmake -S . -B build -DFUNK_LIBREMIDI=ON && cmake --build build`.

- **Sound.** Each file plays through `dirt` with `dsp.superDirt`, as SuperDirt does, at its level, with its limiter on each orbit ([docs/music.md](../../docs/music.md#events-as-superdirt-plays-them)). `dsp.kit` would clip: it plays 8 dB louder, without a limiter, and `cycles-05` peaked at 4.6 times full scale through it.
- **Tempo.** The hosts play 0.5 cycles per second; Tidal's default is 0.5625. `funk-midi --cycle 1778` plays at Tidal's.
- **Missing samples.** A name not in Dirt-Samples is silent. `cycles-03`'s `nn` is not in Dirt-Samples.

`tests/tidal/ports.fk` holds the same patterns for the tests. `lib/tidal-examples` checks that the two match.
