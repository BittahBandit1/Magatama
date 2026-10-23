#!/bin/zsh -f
# Run the Magatama build (Nocturne) straight from the build tree, the way Fukami.app launches its game process.
#   scripts/magatama_run.sh [disc-folder]     (default ~/magatama/disc, which must hold SLUS_209.11 and the
#                                              rest of the disc's files: the ELF's folder is the cdrom0: root)
# Output goes to the terminal and to ~/magatama/logs/run-<time>.log. Extra RRV_* variables pass through.
set -eu
repo="${0:A:h:h}"
disc="${1:-$HOME/magatama/disc}"
[[ -f "$disc/SLUS_209.11" ]] || { print -u2 "no SLUS_209.11 in $disc"; exit 1; }
mkdir -p "$HOME/magatama/memcard" "$HOME/magatama/logs"
log="$HOME/magatama/logs/run-$(date +%Y%m%d-%H%M%S).log"
export RRV_GATE3_BOUND_WORKLOAD="${RRV_GATE3_BOUND_WORKLOAD:-$repo/config/magatama/bound-workload.txt}"
export RRV_GATE3_MC_ROOT="$HOME/magatama/memcard"
export RRV_GATE3_MC_PERSISTENT=1
export RRV_GATE3_LIVE_PAD="${RRV_GATE3_LIVE_PAD:-1}"
export RRV_GIF_PATH_LATENCY="${RRV_GIF_PATH_LATENCY:-0}"   # the RR5 hold keeps Nocturne's FINISH packet from the GS   # 0 for a replay (the workload carries the pad)
export RRV_GATE3_REALTIME=1
# Producer owns GS SIGNAL/FINISH/LABEL; the renderer does not generate these.
export RRV_GS_CONTROL="${RRV_GS_CONTROL:-producer}"
export RRV_PCSX2_GS_VSYNC=1
# Fukami.app's defaults: render the GS on its own worker thread and run VU1 asynchronously, so the cost of drawing a
# frame does not land on the game thread and make frame times uneven. RRV_GS_EXECUTION=inline restores the old path.
export RRV_GS_EXECUTION="${RRV_GS_EXECUTION:-worker-sync}"
[[ "$RRV_GS_EXECUTION" == inline ]] && unset RRV_GS_EXECUTION || export RRV_VU1GS_EXECUTION="${RRV_VU1GS_EXECUTION:-owner-async}"
export RRV_GS_RENDER_MODE="${RRV_GS_RENDER_MODE:-full}"
export RRV_PCSX2_GS_FULL_SCALE="${RRV_PCSX2_GS_FULL_SCALE:-4}"
# Decode movies through the software IPU instead of placeholder MMIO results.
export MAGATAMA_IPU="${MAGATAMA_IPU:-1}"
# macOS Metal performance HUD (the per-app toggle only applies to .app bundles; this is a bare executable).
[[ "${MAGATAMA_METAL_HUD:-1}" == 1 ]] && export MTL_HUD_ENABLED=1   # on by default; MAGATAMA_METAL_HUD=0 hides it
print "== log: $log"
# The runtime finds the PCSX2 GS bridge package only from the fixed layout B/bin/<program> (with the package in
# B/.rrv-resource-packages), so run a copy placed in build/magatama/bin rather than candidate-bin itself.
build="$repo/build/magatama"
mkdir -p "$build/bin"
if [[ ! -f "$build/bin/Fukami" || "$build/candidate-bin/Fukami" -nt "$build/bin/Fukami" ]]; then
    # Replace the file rather than overwrite it in place: macOS caches a binary's code signature per file, and an
    # in-place overwrite makes the kernel kill the new build at launch (zsh: killed, exit 137).
    rm -f "$build/bin/Fukami"
    cp -p "$build/candidate-bin/Fukami" "$build/bin/Fukami"
    codesign --force --sign - "$build/bin/Fukami" 2>/dev/null || true
fi
"$build/bin/Fukami" "$disc/SLUS_209.11" 2>&1 | tee "$log"
