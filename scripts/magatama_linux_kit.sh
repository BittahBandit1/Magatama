#!/bin/zsh -f
# Pack what a Linux machine needs to build Magatama (scripts/magatama_build_linux.sh) into one archive:
#   ~/magatama/Magatama-linux-kit-<commit>.tar.gz
#     magatama/runtime              this checkout as it is now (with .git; no build/, no links into ~/fukami)
#     magatama/generated-nocturne   the recompiled game code
# The disc folder is not included: copy your own disc files (SLUS_209.11 and the rest) to ~/magatama/disc there.
# On Linux:   cd ~ && tar xzf Magatama-linux-kit-*.tar.gz && ~/magatama/runtime/scripts/magatama_build_linux.sh --packages
set -eu
repo="${0:A:h:h}"
top="${repo:h}"
commit="$(git -C "$repo" rev-parse --short=7 HEAD)"
dirty=""; [[ -n "$(git -C "$repo" status --porcelain=v1 --untracked-files=no)" ]] && dirty="-dirty"
out="$top/Magatama-linux-kit-$commit$dirty.tar.gz"
[[ -f "$top/generated-nocturne/register_functions.cpp" ]] || { print -u2 "no $top/generated-nocturne"; exit 1; }
print "== packing $out"
# COPYFILE_DISABLE: no ._* AppleDouble files (macOS metadata) in the archive; some build steps read every file.
COPYFILE_DISABLE=1 tar -C "${top:h}" -czf "$out" \
    --exclude='.DS_Store' \
    --exclude="${top:t}/runtime/build" \
    --exclude="${top:t}/runtime/build-deps" \
    --exclude="${top:t}/runtime/runtime-deps" \
    --exclude="${top:t}/runtime/generated/nocturne/output" \
    --exclude="${top:t}/runtime/local" \
    --exclude='*.log' \
    "${top:t}/runtime" "${top:t}/generated-nocturne"
ls -lh "$out"
