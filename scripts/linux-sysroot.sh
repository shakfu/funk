#!/bin/sh
# linux-sysroot.sh DIR -- unpacks Debian bookworm amd64's X11, OpenGL and ALSA
# headers and libraries into DIR, for scripts/zig-linux.cmake. Packages are found
# in the current index, so point releases do not break the URLs.
set -eu
mkdir -p "$1"
dir=$(cd "$1" && pwd)
mirror=https://deb.debian.org/debian
pkgs="libx11-dev libx11-6 x11proto-dev libxcb1-dev libxau-dev libgl-dev libgl1 libglx-dev
libglx0 libopengl-dev libopengl0 libegl-dev libasound2-dev libasound2"
mkdir -p "$dir/debs"
cd "$dir/debs"
curl -fsSL "$mirror/dists/bookworm/main/binary-amd64/Packages.xz" | xz -d > Packages
for p in $pkgs; do
  f=$(awk -v p="$p" '$1 == "Package:" { x = ($2 == p) } x && $1 == "Filename:" { print $2; exit }' Packages)
  [ -n "$f" ] || { echo "linux-sysroot.sh: $p is not in the index" >&2; exit 1; }
  curl -fsSL -o pkg.deb "$mirror/$f"
  rm -rf x && mkdir x && (cd x && ar x ../pkg.deb && tar -xf data.tar.* -C "$dir")
done
rm -rf x pkg.deb
