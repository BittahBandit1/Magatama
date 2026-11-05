#!/usr/bin/env bash
# Build Magatama (Nocturne) natively on Linux x86-64 or ARM64 (aarch64): the Linux twin of
# scripts/magatama_build.sh. Runs on the Linux machine itself (no Docker, no Mac).
#
#   scripts/magatama_build_linux.sh [--packages] [jobs]
#
#   --packages   install the distribution packages first (apt, dnf or pacman; asks for sudo once)
#
# Layout it expects (what scripts/magatama_linux_kit.sh packs on the Mac):
#   ~/magatama/runtime               this repository (a git checkout: the bridge is built from its HEAD)
#   ~/magatama/generated-nocturne    the recompiled game code (MAGATAMA_GENERATED overrides)
#
# Steps, each skipped when its result already exists (re-run after a failure and it resumes):
#   1. dependency prefix   ~/.local/share/magatama/deps-<arch>  (tools/linux-build/build-deps.sh: SDL 2.32.10,
#                          SDL3, Vulkan headers, shaderc, zstd, lz4, png, jpeg, webp, plutosvg, rapidyaml)
#   2. pinned sources      build-deps/pcsx2-2.8.2 (+ build-deps/sse2neon-92f6de1 on ARM64)
#   3. GS bridge           build/linux/bridge-<commit> from a clean clone of HEAD (Vulkan; 20-60 minutes)
#   4. game code links     generated/nocturne/output -> the generated folder
#   5. product             build/magatama/candidate-bin/Fukami
# Everything is logged to build/linux/build-<time>.log; send that file back if a step fails.
set -euo pipefail
repo=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
packages=0
if [[ "${1:-}" == --packages ]]; then packages=1; shift; fi
jobs="${1:-$(nproc)}"
arch=$(uname -m)
case "$arch" in
    x86_64) ;;
    aarch64|arm64) arch=aarch64 ;;
    *) echo "unsupported CPU: $arch (x86_64 or aarch64)" >&2; exit 1 ;;
esac
[[ $(uname -s) == Linux ]] || { echo "this script is for Linux; on the Mac use scripts/magatama_build.sh" >&2; exit 1; }
mkdir -p "$repo/build/linux"
log="$repo/build/linux/build-$(date +%Y%m%d-%H%M%S).log"
exec > >(tee "$log") 2>&1
echo "== Magatama Linux build ($arch, $jobs jobs) log: $log"

# ---------------------------------------------------------------------------------------------------- packages
if (( packages )); then
    if command -v apt-get >/dev/null; then
        # tools/linux-build/Dockerfile's list (Ubuntu 24.04 / Debian 12 names).
        sudo apt-get update
        sudo apt-get install -y --no-install-recommends \
            build-essential clang lld llvm cmake ninja-build git python3 python3-venv \
            pkg-config curl ca-certificates patch autoconf automake libtool nasm file patchelf \
            libfreetype-dev libfontconfig-dev libx11-dev libx11-xcb-dev libxrandr-dev libxi-dev \
            libxext-dev libxcursor-dev libxinerama-dev libxss-dev libxfixes-dev libxtst-dev \
            libxkbcommon-dev libwayland-dev wayland-protocols libdecor-0-dev libegl-dev libgl-dev \
            libdbus-1-dev libudev-dev libasound2-dev libpulse-dev libpipewire-0.3-dev \
            libcurl4-openssl-dev libpcap-dev zlib1g-dev extra-cmake-modules libssl-dev \
            libvulkan-dev mesa-vulkan-drivers vulkan-tools
    elif command -v dnf >/dev/null; then
        # Fedora (ArmadaOS is Fedora 44; build inside its distrobox, scripts/magatama_armada.sh).
        sudo dnf -y install --setopt=install_weak_deps=False \
            gcc-c++ clang lld llvm cmake ninja-build git python3 pkgconf-pkg-config curl patch autoconf automake \
            libtool nasm file patchelf which freetype-devel fontconfig-devel libX11-devel libxcb-devel \
            libXrandr-devel libXi-devel libXext-devel libXcursor-devel libXinerama-devel libXScrnSaver-devel \
            libXfixes-devel libXtst-devel libxkbcommon-devel wayland-devel wayland-protocols-devel libdecor-devel \
            mesa-libEGL-devel mesa-libGL-devel dbus-devel systemd-devel alsa-lib-devel pulseaudio-libs-devel \
            pipewire-devel libcurl-devel libpcap-devel zlib-ng-compat-devel extra-cmake-modules openssl-devel \
            vulkan-loader-devel vulkan-headers mesa-vulkan-drivers vulkan-tools
    elif command -v pacman >/dev/null; then
        sudo pacman -S --needed --noconfirm \
            base-devel clang lld llvm cmake ninja git python pkgconf curl patch autoconf automake libtool nasm \
            file patchelf freetype2 fontconfig libx11 libxrandr libxi libxext libxcursor libxinerama libxss \
            libxfixes libxtst libxkbcommon wayland wayland-protocols libdecor libglvnd dbus systemd-libs \
            alsa-lib libpulse pipewire curl libpcap zlib extra-cmake-modules openssl \
            vulkan-headers vulkan-icd-loader vulkan-tools
    else
        echo "no apt-get, dnf or pacman: install the packages listed in tools/linux-build/Dockerfile by hand" >&2
        exit 1
    fi
fi
for tool in clang clang++ cmake ninja git python3 curl pkg-config; do
    command -v "$tool" >/dev/null || { echo "missing $tool: run again with --packages" >&2; exit 1; }
done
export CC=clang CXX=clang++
command -v ld.lld >/dev/null && export LDFLAGS="${LDFLAGS:-} -fuse-ld=lld"

# ------------------------------------------------------------------------------------------- dependency prefix
deps="${MAGATAMA_DEPS:-$HOME/.local/share/magatama/deps-$arch}"
if [[ ! -f "$deps/.magatama-deps-done" ]]; then
    echo "== dependencies -> $deps (about 20-40 minutes, once)"
    mkdir -p "$deps"
    bash "$repo/tools/linux-build/build-deps.sh" "$deps"
    touch "$deps/.magatama-deps-done"
fi
export PKG_CONFIG_PATH="$deps/lib/pkgconfig:${PKG_CONFIG_PATH:-}"
export CMAKE_PREFIX_PATH="$deps${CMAKE_PREFIX_PATH:+:$CMAKE_PREFIX_PATH}"
export LD_LIBRARY_PATH="$deps/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

# ---------------------------------------------------------------------------------------------- pinned sources
# On the Mac build-deps and runtime-deps are links into ~/fukami; here they are real folders.
for d in build-deps runtime-deps; do
    if [[ -L "$repo/$d" && ! -e "$repo/$d" ]]; then rm "$repo/$d"; fi
    mkdir -p "$repo/$d"
done
[[ -d "$repo/build-deps/pcsx2-2.8.2/.git" ]] ||
    python3 "$repo/scripts/prepare_dependencies.py" --prepare --component pcsx2
sse2neon_args=()
if [[ $arch == aarch64 ]]; then
    [[ -f "$repo/build-deps/sse2neon-92f6de1/sse2neon.h" ]] ||
        python3 "$repo/scripts/prepare_dependencies.py" --prepare --component sse2neon
    sse2neon_args=(-DFETCHCONTENT_SOURCE_DIR_SSE2NEON="$repo/build-deps/sse2neon-92f6de1")
fi

# -------------------------------------------------------------------------------------------------- GS bridge
# The bridge manifest binds the library to a clean checkout, so it is built from a private clone of HEAD (the
# bridge code is not part of uncommitted game work). Same recipe as tools/linux-build/build_linux_runtime.sh.
commit=$(git -C "$repo" rev-parse --short=7 HEAD)
root="$repo/build/linux/root-$commit"
bridge="$repo/build/linux/bridge-$commit"
if [[ ! -d "$root" ]]; then
    git clone -q --no-local --no-checkout "$repo" "$root"
    git -C "$root" checkout -q --detach "$commit"
fi
if [[ ! -f "$bridge/rrv-pcsx2-gs-bridge-manifest.json" ]]; then
    echo "== GS bridge -> $bridge (Vulkan; 20-60 minutes)"
    rm -rf "$bridge" "$bridge"-source-*
    (cd "$root" && CMAKE_BUILD_TYPE=Release CMAKE_BUILD_PARALLEL_LEVEL="$jobs" \
        PCSX2_GS_BRIDGE_BUILD_DIR="$bridge" PCSX2_SOURCE_DIR="$repo/build-deps/pcsx2-2.8.2" \
        scripts/build_pcsx2_gs_bridge.sh -DCMAKE_PREFIX_PATH="$deps" -DCMAKE_DISABLE_FIND_PACKAGE_FFMPEG=ON \
        ${sse2neon_args[@]+"${sse2neon_args[@]}"})
fi
export MAGATAMA_BRIDGE_ROOT="$root"

# ---------------------------------------------------------------------------------------------------- game code
generated="${MAGATAMA_GENERATED:-$(dirname "$repo")/generated-nocturne}"
[[ -f "$generated/register_functions.cpp" ]] || {
    echo "no recompiled game code at $generated (copy generated-nocturne from the Mac, or set MAGATAMA_GENERATED)" >&2
    exit 1; }
mkdir -p "$repo/generated/nocturne"
ln -sfn "$generated" "$repo/generated/nocturne/output"
for need in source-manifest.json vu legacy-stubs/ps2_recompiled_stubs.h; do
    [[ -e "$repo/generated/nocturne/$need" ]] || {
        echo "generated/nocturne/$need is missing: copy generated/nocturne from the Mac build (the kit has it)" >&2
        exit 1; }
done
mkdir -p "$repo/generated/nocturne/native"

# ------------------------------------------------------------------------------------------------------ product
build="$repo/build/magatama"
echo "== product -> $build"
cmake -S "$repo" -B "$build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF \
    -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
    -DCMAKE_PREFIX_PATH="$deps" -DRRV_LINUX_SDL_PREFIX="$deps" \
    -DRRV_BUILD_PRODUCT=ON -DRRV_PRODUCT_OWNED_SOURCE=ON -DRRV_GS_PRODUCER_CONTROL=ON \
    -DMAGATAMA_GAME=ON \
    -DRRV_PCSX2_GS_BRIDGE_MANIFEST="$bridge/rrv-pcsx2-gs-bridge-manifest.json" \
    -DRRV_PCSX2_SOURCE_DIR="$repo/build-deps/pcsx2-2.8.2" \
    ${sse2neon_args[@]+"${sse2neon_args[@]}"}
cmake --build "$build" --target Fukami --parallel "$jobs"
echo "== built: $build/candidate-bin/Fukami"
echo "   run:   scripts/magatama_run_linux.sh [disc-folder]"
