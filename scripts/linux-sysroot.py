#!/usr/bin/env python3
"""linux-sysroot.py DIR -- unpacks Debian bookworm amd64's X11, OpenGL and ALSA
headers and libraries into DIR, for scripts/zig-linux.cmake. Packages are found
in the current index, so point releases do not break the URLs."""

import io
import lzma
import os
import sys
import tarfile
import urllib.request

MIRROR = "https://deb.debian.org/debian"
PKGS = """libx11-dev libx11-6 x11proto-dev libxcb1-dev libxau-dev libgl-dev libgl1 libglx-dev
libglx0 libopengl-dev libopengl0 libegl-dev libasound2-dev libasound2""".split()


def fetch(url):
    with urllib.request.urlopen(url) as r:
        return r.read()


def filenames(index):
    """package -> its .deb's path on the mirror; the first stanza wins"""
    out = {}
    for stanza in index.split("\n\n"):
        fields = dict(line.split(": ", 1) for line in stanza.splitlines()
                      if ": " in line and not line.startswith(" "))
        if "Package" in fields and "Filename" in fields:
            out.setdefault(fields["Package"], fields["Filename"])
    return out


def data_tar(deb):
    """the data.tar.* member of an ar archive"""
    if deb[:8] != b"!<arch>\n":
        raise ValueError("not an ar archive")
    i = 8
    while i + 60 <= len(deb):
        name = deb[i:i + 16].decode().strip().rstrip("/")
        size = int(deb[i + 48:i + 58])
        if name.startswith("data.tar"):
            return deb[i + 60:i + 60 + size]
        i += 60 + size + size % 2
    raise ValueError("no data.tar member")


def main():
    d = os.path.abspath(sys.argv[1])
    os.makedirs(d, exist_ok=True)
    index = filenames(lzma.decompress(
        fetch(f"{MIRROR}/dists/bookworm/main/binary-amd64/Packages.xz")).decode())
    for p in PKGS:
        if p not in index:
            print(f"linux-sysroot.py: {p} is not in the index", file=sys.stderr)
            return 1
        with tarfile.open(fileobj=io.BytesIO(data_tar(fetch(f"{MIRROR}/{index[p]}")))) as t:
            t.extractall(d, filter="tar")
    return 0


if __name__ == "__main__":
    sys.exit(main())
