#!/bin/zsh -f
# Configure and build the Magatama product binary (Nocturne) against the PCSX2 GS bridge that the
# Fukami checkout already built. Usage: scripts/magatama_build.sh [jobs]
set -eu
repo="${0:A:h:h}"
fukami="${FUKAMI_DIR:-$HOME/fukami}"
bridge_manifest="$(ls -d "$fukami"/build/fukami/bridge-v1-*/rrv-pcsx2-gs-bridge-manifest.json | head -1)"
build="$repo/build/magatama"
[[ -f "$repo/generated/nocturne/source-manifest.json" ]] || {
    print -u2 "run: python3 scripts/magatama_prepare_generated.py"; exit 1; }
bridge_commit="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["rrv_commit"][:7])' "$bridge_manifest")"
export MAGATAMA_BRIDGE_ROOT="$fukami/build/fukami/root-$bridge_commit"
[[ -d "$MAGATAMA_BRIDGE_ROOT/tools/pcsx2-gs-bridge" ]] || { print -u2 "missing $MAGATAMA_BRIDGE_ROOT"; exit 1; }
print "== bridge: $bridge_manifest (built from $MAGATAMA_BRIDGE_ROOT)"
cmake -S "$repo" -B "$build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF \
    -DRRV_BUILD_PRODUCT=ON -DRRV_PRODUCT_OWNED_SOURCE=ON -DRRV_GS_PRODUCER_CONTROL=ON \
    -DMAGATAMA_GAME=ON \
    -DRRV_PCSX2_GS_BRIDGE_MANIFEST="$bridge_manifest" \
    -DRRV_PCSX2_SOURCE_DIR="${MAGATAMA_PCSX2_SOURCE_DIR:-$fukami/build-deps/pcsx2-2.8.2}" \
    -DFETCHCONTENT_SOURCE_DIR_SSE2NEON="$repo/build-deps/sse2neon-92f6de1"
cmake --build "$build" --target Fukami --parallel "${1:-8}"
print "== built: $build/candidate-bin/Fukami"
