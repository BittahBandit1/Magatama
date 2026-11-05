#!/usr/bin/env bash
# Android (arm64-v8a) dependency prefix for Magatama: everything the PCSX2 GS bridge and the product link, built from
# source with the NDK (there is no distribution behind it). Versions follow tools/linux-build/build-deps.sh; the list of
# what PCSX2 needs on Android follows ARMSX2's .github/workflows/scripts/android/build-dependencies.sh (GPL-3.0).
#
#   tools/android/build-deps.sh <prefix>        (ANDROID_NDK must point at an NDK, r28 or newer)
#
# SDL3 is shared and owns the app (its Java SDLActivity runs the game); sdl2-compat provides libSDL2.so on top of it for
# the product's SDL2 code, so the process has exactly one SDL. The rest are static and PIC.
set -euo pipefail
PREFIX=$(realpath -m "$1"); mkdir -p "$PREFIX"
NDK="${ANDROID_NDK:?set ANDROID_NDK}"
ABI=arm64-v8a; API="${ANDROID_API:-28}"; JOBS="${JOBS:-$(nproc)}"
SDL3=release-3.4.12; SDL2COMPAT=release-2.32.74; LIBPNG=v1.6.51; JPEGTURBO=3.1.4.1; ZSTD=v1.5.7; LZ4=v1.10.0
LIBWEBP=v1.6.0; FREETYPE=VER-2-14-1; PLUTOVG=v1.3.2; PLUTOSVG=v0.0.7; RAPIDYAML=v0.12.1; CURL=curl-8_11_1
SHADERC=v2026.2
TC=(-G Ninja -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" -DANDROID_ABI=$ABI
    -DANDROID_PLATFORM=android-$API -DANDROID_STL=c++_shared -DCMAKE_POSITION_INDEPENDENT_CODE=ON
    -DCMAKE_INSTALL_PREFIX="$PREFIX" -DCMAKE_PREFIX_PATH="$PREFIX" -DCMAKE_FIND_ROOT_PATH="$PREFIX"
    -DCMAKE_INSTALL_LIBDIR=lib -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF -DCMAKE_POLICY_VERSION_MINIMUM=3.5)
mkdir -p "$PREFIX/src" "$PREFIX/stamps"; cd "$PREFIX/src"
clone() { [[ -d "$2" ]] || git clone -q --depth 1 --branch "$3" "$1" "$2"; }
step() { # step <name> <src> [cmake args]: configure, build, install once
    local name=$1 src=$2; shift 2
    [[ -f "$PREFIX/stamps/$name" ]] && { echo "== $name: done"; return; }
    echo "== $name"
    cmake -S "$src" -B "b-$name" "${TC[@]}" "$@" > "b-$name.log" 2>&1 &&
        cmake --build "b-$name" --parallel "$JOBS" >> "b-$name.log" 2>&1 &&
        cmake --install "b-$name" >> "b-$name.log" 2>&1 || { echo "!! $name failed"; tail -30 "b-$name.log"; exit 1; }
    touch "$PREFIX/stamps/$name"
}
clone https://github.com/libsdl-org/SDL.git SDL3 $SDL3
step SDL3 SDL3 -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF -DSDL_TEST_LIBRARY=OFF
clone https://github.com/libsdl-org/sdl2-compat.git sdl2-compat $SDL2COMPAT
step sdl2-compat sdl2-compat -DSDL2COMPAT_TESTS=OFF -DSDL2COMPAT_X11=OFF
clone https://github.com/pnggroup/libpng libpng $LIBPNG
step libpng libpng -DPNG_SHARED=OFF -DPNG_STATIC=ON -DPNG_TESTS=OFF -DPNG_TOOLS=OFF
clone https://github.com/libjpeg-turbo/libjpeg-turbo jpeg $JPEGTURBO
step jpeg jpeg -DENABLE_SHARED=OFF -DENABLE_STATIC=ON -DWITH_TURBOJPEG=OFF
clone https://github.com/facebook/zstd zstd $ZSTD
step zstd zstd/build/cmake -DZSTD_BUILD_SHARED=OFF -DZSTD_BUILD_STATIC=ON -DZSTD_BUILD_PROGRAMS=OFF -DZSTD_BUILD_TESTS=OFF
clone https://github.com/lz4/lz4 lz4 $LZ4
step lz4 lz4/build/cmake -DLZ4_BUILD_CLI=OFF -DLZ4_BUILD_LEGACY_LZ4C=OFF
clone https://github.com/webmproject/libwebp webp $LIBWEBP
step webp webp -DWEBP_BUILD_ANIM_UTILS=OFF -DWEBP_BUILD_CWEBP=OFF -DWEBP_BUILD_DWEBP=OFF -DWEBP_BUILD_GIF2WEBP=OFF \
    -DWEBP_BUILD_IMG2WEBP=OFF -DWEBP_BUILD_VWEBP=OFF -DWEBP_BUILD_WEBPINFO=OFF -DWEBP_BUILD_WEBPMUX=OFF -DWEBP_BUILD_EXTRAS=OFF
clone https://github.com/freetype/freetype freetype $FREETYPE
step freetype freetype -DFT_DISABLE_HARFBUZZ=ON -DFT_DISABLE_BROTLI=ON -DFT_DISABLE_PNG=ON -DFT_DISABLE_ZLIB=ON -DFT_DISABLE_BZIP2=ON
clone https://github.com/sammycage/plutovg plutovg $PLUTOVG
step plutovg plutovg -DPLUTOVG_BUILD_EXAMPLES=OFF
clone https://github.com/sammycage/plutosvg plutosvg $PLUTOSVG
step plutosvg plutosvg -DPLUTOSVG_ENABLE_FREETYPE=ON -DPLUTOSVG_BUILD_EXAMPLES=OFF
[[ -d rapidyaml ]] || git clone -q --depth 1 --branch $RAPIDYAML --recursive https://github.com/biojppm/rapidyaml rapidyaml
step rapidyaml rapidyaml
clone https://github.com/curl/curl curl $CURL
step curl curl -DCURL_ENABLE_SSL=OFF -DBUILD_CURL_EXE=OFF -DCURL_DISABLE_LDAP=ON -DCURL_DISABLE_LDAPS=ON \
    -DCURL_USE_LIBPSL=OFF -DBUILD_TESTING=OFF -DBUILD_EXAMPLES=OFF
# libpcap: not built (needs flex/bison); PCSX2 DEV9 links the pcap stub from the iOS port (tools/patches/android).
if [[ ! -f "$PREFIX/stamps/shaderc" ]]; then
    clone https://github.com/google/shaderc shaderc $SHADERC
    (cd shaderc && [[ -d third_party/glslang ]] || python3 utils/git-sync-deps)
    echo "== shaderc"
    cmake -S shaderc -B b-shaderc "${TC[@]}" -DSHADERC_SKIP_TESTS=ON -DSHADERC_SKIP_EXAMPLES=ON \
        -DSHADERC_SKIP_COPYRIGHT_CHECK=ON > b-shaderc.log 2>&1 &&
        cmake --build b-shaderc --parallel "$JOBS" --target shaderc_combined >> b-shaderc.log 2>&1 ||
        { echo "!! shaderc failed"; tail -30 b-shaderc.log; exit 1; }
    cp b-shaderc/libshaderc/libshaderc_combined.a "$PREFIX/lib/"; cp -r shaderc/libshaderc/include/shaderc "$PREFIX/include/"
    touch "$PREFIX/stamps/shaderc"
fi
echo "== Android ($ABI, API $API) dependencies in $PREFIX"
