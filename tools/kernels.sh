#!/bin/sh
# kernels.sh FUNK FILE OUT -- compiles FILE's foreign exports into the
# shared library OUT, without the runtime (docs/dev/audio.md, D), linking
# the libraries named on the C's first line. $CC and $CFLAGS apply.
set -eu
funk=$1 src=$2 out=$3
c=$out.c
"$funk" emit-c --kernels "$src" -o "$c"
libs=$(sed -n '1s|^/\* link: \(.*\) \*/$|\1|p' "$c")
# shellcheck disable=SC2086
${CC:-cc} -O2 -fPIC -shared ${CFLAGS:-} -o "$out" "$c" $libs
rm -f "$c"
