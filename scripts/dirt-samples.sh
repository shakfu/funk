#!/bin/sh
# dirt-samples.sh [DIR] -- fetches Tidal's Dirt-Samples, about 400 MB, at a
# pinned commit into DIR, by default ~/.local/share/funk/Dirt-Samples, the
# folder examples/tidal's kits read. It has no license file, so funk does not
# ship it. DIRT_SAMPLES_URL overrides where it is fetched from.
set -eu
url=${DIRT_SAMPLES_URL:-https://github.com/tidalcycles/Dirt-Samples}
pin=c74fc80f8db8038f6a33648ffef5ac00a07ad402
dir=${1:-$HOME/.local/share/funk/Dirt-Samples}
if [ -e "$dir" ]; then
  if [ "$(git -C "$dir" rev-parse HEAD 2>/dev/null)" = "$pin" ]; then
    echo "$dir: already at ${pin%"${pin#???????}"}"
    exit 0
  fi
  echo "dirt-samples.sh: $dir exists and is not Dirt-Samples at ${pin%"${pin#???????}"}; remove it or name another" >&2
  exit 1
fi
mkdir -p "$(dirname "$dir")"
tmp="$dir.part"
rm -rf "$tmp"
trap 'rm -rf "$tmp"' EXIT
git init -q "$tmp"
git -C "$tmp" fetch -q --depth 1 "$url" "$pin"
git -C "$tmp" -c advice.detachedHead=false checkout -q FETCH_HEAD
mv "$tmp" "$dir"
trap - EXIT
echo "$dir: Dirt-Samples at ${pin%"${pin#???????}"}"
