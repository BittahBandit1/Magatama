#!/usr/bin/env bash
# ArmadaOS (Fedora 44, ARM64 handhelds such as the Retroid Pocket 5): build and run Magatama.
# The system image is read-only, so the compiler and -devel packages live in a Fedora 44 distrobox
# ("magatama-build"); the finished game runs on the host (its own libraries are bundled in
# ~/.local/share/magatama/deps-aarch64, Vulkan comes from Armada's Mesa/Turnip).
#
#   ~/magatama/runtime/scripts/magatama_armada.sh build   first build (about 1-3 hours on a Snapdragon 865), resumable
#   ~/magatama/runtime/scripts/magatama_armada.sh run     play (disc files in ~/magatama/disc)
#
# Logs: ~/magatama/runtime/build/linux/build-<time>.log and ~/magatama/logs/run-<time>.log.
set -euo pipefail
repo=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
box=magatama-build
case "${1:-}" in
build)
    command -v distrobox >/dev/null || { echo "distrobox is missing (ArmadaOS ships it)" >&2; exit 1; }
    if ! distrobox list 2>/dev/null | grep -q " $box "; then
        echo "== creating the $box distrobox (Fedora 44)"
        distrobox create --yes --name "$box" --image registry.fedoraproject.org/fedora:44
    fi
    # Leave a core free so the handheld stays usable; ninja jobs = cores - 1.
    jobs=$(( $(nproc) > 2 ? $(nproc) - 1 : 1 ))
    distrobox enter "$box" -- bash -lc "'$repo/scripts/magatama_build_linux.sh' --packages $jobs"
    ;;
run)
    shift
    # Prefer the host (Armada's own Mesa/Turnip); if a library the build needs is not on the host image,
    # run inside the build box instead, which has every package it was built against.
    deps="${MAGATAMA_DEPS:-$HOME/.local/share/magatama/deps-aarch64}"
    missing=$(for f in "$repo/build/magatama/candidate-bin/Fukami" \
                       $(find "$repo/build/magatama/.rrv-resource-packages/" -name '*.so*' 2>/dev/null); do
                  LD_LIBRARY_PATH="$deps/lib" ldd "$f" 2>/dev/null | grep "not found"; done | sort -u)
    if [[ -z "$missing" || "${MAGATAMA_HOST:-}" == 1 ]]; then
        exec "$repo/scripts/magatama_run_linux.sh" "$@"
    fi
    echo "== not on the host image, running inside $box:"; echo "$missing"
    exec distrobox enter "$box" -- "$repo/scripts/magatama_run_linux.sh" "$@"
    ;;
*)
    sed -n '2,11p' "$0"; exit 1 ;;
esac
