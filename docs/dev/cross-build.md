# Cross builds with zig cc

The Linux CLAP plugin builds on macOS with `zig cc`. CI's `plugin-zig` job does this, and `plugin-zig-run` runs the result on Linux in `test_clap_host`.

```sh
make                                          # the host tools: build/funk, build/embed
scripts/linux-sysroot.py /tmp/sysroot         # X11, OpenGL, ALSA from Debian bookworm
FUNK_SYSROOT=/tmp/sysroot cmake -S . -B xbuild -G Ninja -DCMAKE_TOOLCHAIN_FILE=scripts/zig-linux.cmake \
  -DFUNK_CLAP=ON -DLIBREMIDI_NO_UDEV=ON -DFUNK_HOST_TOOLS=$PWD/build
cmake --build xbuild --target funk_midi_clap
```

Zig provides glibc, but not X11, OpenGL or ALSA, hence the sysroot. udev is off to keep libudev out of it. Tried with Zig 0.17.1 and 0.18.0-dev.930.

## What differs from a native build

- `-Wl,--exclude-libs,ALL` is dropped: Zig rejects it. The version script still exports only `clap_entry`.
- libfunk links `-lunwind`. `funk build` with `CC="zig cc -target ...-linux-gnu"` needs `-lunwind` too, for `rt.c`'s GC check.
- libc++ is linked statically. The plugin needs `libX11.so.6`, `libGLX.so.0`, `libOpenGL.so.0` and glibc 2.34; it is 2.3 MB stripped.
