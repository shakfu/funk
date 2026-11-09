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

- **Level.** The patches sum many samples at full level. Peaks reach 2 to 9.5 times full scale (`cycles-11`), as they would in SuperDirt. Turn the output down, or add `# gain "0.8"` to `pat`.
- **Tempo.** The hosts play 0.5 cycles per second; Tidal's default is 0.5625. `funk-midi --cycle 1778` plays at Tidal's.
- **Missing samples.** A name not in the kit is silent. `cycles-03`'s `nn` is not in Dirt-Samples.

`tests/tidal/ports.fk` holds the same patterns for the tests. `lib/tidal-examples` checks that the two match.
