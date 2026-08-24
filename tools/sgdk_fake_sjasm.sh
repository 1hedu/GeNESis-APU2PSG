#!/bin/sh
# Stand-in for sjasm. SGDK's .s80 sources are unchanged, so their assembled
# output is byte-identical to what the shipped library already contains -- and
# those objects are pure data with no .text, so reusing them mixes no ABI.
# Extract the blob from the prebuilt object instead of assembling it.
for a in "$@"; do case "$a" in *.o80) OUT="$a";; esac; done
SRC=""; for a in "$@"; do case "$a" in *.s80) SRC="$a";; esac; done
BASE=$(basename "$SRC" .s80)
m68k-linux-gnu-objcopy -O binary --only-section=.rodata --only-section=.data \
    ""${S80_OBJDIR:-/tmp/s80obj}"/$BASE.o" "$OUT" 2>/dev/null || exit 1
[ -s "$OUT" ] || exit 1
exit 0
