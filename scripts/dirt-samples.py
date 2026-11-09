#!/usr/bin/env python3
"""dirt-samples.py [DIR] -- fetches Tidal's Dirt-Samples, about 400 MB, at a
pinned commit into DIR, by default ~/.local/share/funk/Dirt-Samples, the folder
examples/tidal's kits read. It has no license file, so funk does not ship it.
DIRT_SAMPLES_URL overrides where it is fetched from."""

import os
import shutil
import subprocess
import sys

URL = os.environ.get("DIRT_SAMPLES_URL", "https://github.com/tidalcycles/Dirt-Samples")
PIN = "c74fc80f8db8038f6a33648ffef5ac00a07ad402"


def git(*args):
    subprocess.run(["git", *args], check=True)


def main():
    home = os.path.expanduser("~/.local/share/funk/Dirt-Samples")
    d = sys.argv[1] if len(sys.argv) > 1 else home
    if os.path.lexists(d):
        head = subprocess.run(["git", "-C", d, "rev-parse", "HEAD"],
                              capture_output=True, text=True).stdout.strip()
        if head == PIN:
            print(f"{d}: already at {PIN[:7]}")
            return 0
        print(f"dirt-samples.py: {d} exists and is not Dirt-Samples at {PIN[:7]}; "
              "remove it or name another", file=sys.stderr)
        return 1
    os.makedirs(os.path.dirname(os.path.abspath(d)), exist_ok=True)
    tmp = d + ".part"
    shutil.rmtree(tmp, ignore_errors=True)
    try:
        git("init", "-q", tmp)
        git("-C", tmp, "fetch", "-q", "--depth", "1", URL, PIN)
        git("-C", tmp, "-c", "advice.detachedHead=false", "checkout", "-q", "FETCH_HEAD")
        os.rename(tmp, d)
    except BaseException:
        shutil.rmtree(tmp, ignore_errors=True)
        raise
    print(f"{d}: Dirt-Samples at {PIN[:7]}")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except subprocess.CalledProcessError as e:
        sys.exit(e.returncode)
