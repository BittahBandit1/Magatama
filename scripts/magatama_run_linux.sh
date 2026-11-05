#!/usr/bin/env bash
# Run the Linux Magatama build (scripts/magatama_build_linux.sh) from the build tree: the Linux twin of
# scripts/magatama_run.sh.
#   scripts/magatama_run_linux.sh [disc-folder]   (default ~/magatama/disc: SLUS_209.11 and the rest of the
#                                                  disc's files; the ELF's folder is the cdrom0: root)
# Saves go to ~/magatama/memcard, settings to ~/magatama/magatama.ini, logs to ~/magatama/logs/run-<time>.log.
# Extra RRV_* / MAGATAMA_* variables pass through.
set -euo pipefail
repo=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
disc="${1:-$HOME/magatama/disc}"
[[ -f "$disc/SLUS_209.11" ]] || { echo "no SLUS_209.11 in $disc" >&2; exit 1; }
arch=$(uname -m); [[ $arch == arm64 ]] && arch=aarch64
deps="${MAGATAMA_DEPS:-$HOME/.local/share/magatama/deps-$arch}"
export LD_LIBRARY_PATH="$deps/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
mkdir -p "$HOME/magatama/memcard" "$HOME/magatama/logs"
log="$HOME/magatama/logs/run-$(date +%Y%m%d-%H%M%S).log"

export RRV_GATE3_BOUND_WORKLOAD="${RRV_GATE3_BOUND_WORKLOAD:-$repo/config/magatama/bound-workload.txt}"
export RRV_GATE3_MC_ROOT="${RRV_GATE3_MC_ROOT:-$HOME/magatama/memcard}"
export RRV_GATE3_MC_PERSISTENT=1
export RRV_GATE3_LIVE_PAD="${RRV_GATE3_LIVE_PAD:-1}"
export RRV_GIF_PATH_LATENCY="${RRV_GIF_PATH_LATENCY:-0}"
export RRV_GATE3_REALTIME=1
export MAGATAMA_SILENT_SOUND="${MAGATAMA_SILENT_SOUND:-1}"
export MAGATAMA_SOUND="${MAGATAMA_SOUND:-1}"
export MAGATAMA_DISC="$disc"
export RRV_GS_CONTROL="${RRV_GS_CONTROL:-producer}"
export RRV_PCSX2_GS_VSYNC=1
export RRV_GS_EXECUTION="${RRV_GS_EXECUTION:-worker-sync}"
if [[ "$RRV_GS_EXECUTION" == inline ]]; then unset RRV_GS_EXECUTION
else export RRV_VU1GS_EXECUTION="${RRV_VU1GS_EXECUTION:-owner-async}"; fi
export RRV_FUKAMI_INI="${RRV_FUKAMI_INI:-$HOME/magatama/magatama.ini}"
export MAGATAMA_IPU="${MAGATAMA_IPU:-1}"
export MAGATAMA_IPU_WORKER_TIMING="${MAGATAMA_IPU_WORKER_TIMING:-1}"
export RRV_VU_AOT="${RRV_VU_AOT:-1}"
[[ "${MAGATAMA_PRESENT_TRACE:-0}" == 1 ]] && export RRV_PCSX2_GS_PRESENT_TRACE="${log%.log}.present.txt"
[[ "${MAGATAMA_INTERP_PROFILE:-0}" == 1 ]] && export MAGATAMA_INTERP_PROFILE=1

echo "== log: $log"
# The runtime finds the GS bridge package only from the fixed layout B/bin/<program>
# (package in B/.rrv-resource-packages), so run a copy in build/magatama/bin.
build="$repo/build/magatama"
mkdir -p "$build/bin"
if [[ ! -f "$build/bin/Fukami" || "$build/candidate-bin/Fukami" -nt "$build/bin/Fukami" ]]; then
    rm -f "$build/bin/Fukami"
    cp -p "$build/candidate-bin/Fukami" "$build/bin/Fukami"
fi
export MAGATAMA_RESTART_LAUNCHER=1
set +e
"$build/bin/Fukami" "$disc/SLUS_209.11" 2>&1 | tee "$log"
status=("${PIPESTATUS[@]}")
set -e
# 75: the in-game menu asked for a restart after a clean shutdown.
if [[ "${status[0]}" == 75 && "${status[1]}" == 0 ]]; then exec "$0" "$disc"; fi
[[ "${status[0]}" == 0 ]] || exit "${status[0]}"
exit "${status[1]}"
