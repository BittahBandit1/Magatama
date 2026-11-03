#!/bin/zsh -f
# Build the isolated original-driver audio test; never starts a game or mounts a card.
set -eu
repo="${0:A:h:h}"
disc="${MAGATAMA_DISC_ROOT:-${repo:h}/disc}"
cmake -S "$repo/tools/nocturne-iop" -B "$repo/build/nocturne-iop" -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build "$repo/build/nocturne-iop" --parallel 4
args=()
if (( $# > 0 )); then args+=(--wav "$1"); fi
python3 "$repo/tools/nocturne-iop/verify.py" "$repo/build/nocturne-iop/nocturne-iop-probe" "$disc" "${args[@]}"
