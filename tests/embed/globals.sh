#!/bin/sh
# globals.sh ALLOW OBJ...: libfunk's writable variables are ALLOW's, so a new
# one fails (docs/dev/embedding.md, phase 2): a session's state belongs in its
# FkRt or its compiler's structs. OBJ are built at -O0, where clang merges no
# globals into one symbol.
allow=$1
shift
if [ "$(uname)" = Darwin ]; then
  nm -m "$@" | awk '/__DATA,__(data|bss|common)|\(common\)|__thread_(bss|data|vars)/ && !/__const/ {print $NF}'
else # by section: with -fPIC, a const table of pointers is in .data.rel.ro, which nm also calls d
  nm -f sysv "$@" | awk -F'|' 'NF >= 7 {
    name = $1; sec = $7; gsub(/ /, "", name); gsub(/ /, "", sec)
    if ((sec ~ /^\.(data|bss|tdata|tbss)(\.|$)/ && sec !~ /rel\.ro/) || sec == "*COM*") print name
  }'
fi | sed -e 's/^_//' -e 's/\$tlv\$init$//' |
  awk -F. '{ n = $0; for (i = NF; i > 0; i--) if ($i !~ /^[0-9]+$/) { n = $i; break } print n }' |
  grep -v '^ltmp[0-9]*$' | sort -u | diff "$allow" -
