#!/usr/bin/env bash
# Build Magatama for Android (arm64-v8a) and pack an installable APK, on a Linux x86-64 or macOS host with an NDK.
#
#   ANDROID_NDK=<ndk r28+> ANDROID_SDK=<sdk with build-tools + platforms> scripts/magatama_build_android.sh [stage]
#
# Stages (each resumes; name one to start there): deps pcsx2 bridge product apk. Output:
#   build/android/Magatama-android-b<N>.apk     (debug-signed; N from ~/magatama/android-build-number, or
#                                                 MAGATAMA_ANDROID_BUILD_NO)
# Layout inside the APK:
#   lib/arm64-v8a/  libSDL3.so libSDL2.so (sdl2-compat) libmain.so (the game) librrv-pcsx2-gs-bridge.so libc++_shared.so
#   assets/game/    package/ (identity, PCSX2 resources, bound-workload.txt) + package.list (extracted on first launch)
#   classes.dex     SDL3's Java activity (org.libsdl.app)
set -euo pipefail
repo=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
NDK="${ANDROID_NDK:?set ANDROID_NDK to an NDK (r28 or newer)}"
SDK="${ANDROID_SDK:?set ANDROID_SDK to an SDK folder with build-tools/<v> and platforms/android-<n>}"
out="${MAGATAMA_ANDROID_OUT:-$repo/build/android}"; mkdir -p "$out"
deps="$out/deps"; JOBS="${JOBS:-$(nproc 2>/dev/null || sysctl -n hw.ncpu)}"
API=28; TARGET_SDK=35; ABI=arm64-v8a
start="${1:-deps}"
stages=(deps pcsx2 bridge product apk)
run_from() { local s; for s in "${stages[@]}"; do [[ $s == "$start" ]] && return 0; [[ $s == "$1" ]] && return 1; done; return 0; }
log() { echo "== $*"; }
TC=(-G Ninja -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" -DANDROID_ABI=$ABI
    -DANDROID_PLATFORM=android-$API -DANDROID_STL=c++_shared -DCMAKE_BUILD_TYPE=Release
    -DCMAKE_PREFIX_PATH="$deps" -DCMAKE_FIND_ROOT_PATH="$deps")

# ---------------------------------------------------------------------------------------------------- 1. deps
if run_from deps; then
    log "dependencies -> $deps"
    ANDROID_NDK="$NDK" JOBS="$JOBS" ANDROID_API=$API "$repo/tools/android/build-deps.sh" "$deps"
fi

# ---------------------------------------------------------------------------------------- 2. patched PCSX2 tree
# The locked bridge patch, the Linux (Vulkan accounting) patch and the Android patch, on the pinned revision.
pcsx2="$out/pcsx2"
if run_from pcsx2; then
    log "PCSX2 source -> $pcsx2"
    rev="$(awk '$0=="[pcsx2]"{p=1;next} p&&/^revision = /{gsub(/"/,"",$3);print $3;exit}' "$repo/config/dependencies.lock.toml")"
    [[ -d "$repo/build-deps/pcsx2-2.8.2/.git" ]] || python3 "$repo/scripts/prepare_dependencies.py" --prepare --component pcsx2
    [[ -f "$repo/build-deps/sse2neon-92f6de1/sse2neon.h" ]] || python3 "$repo/scripts/prepare_dependencies.py" --prepare --component sse2neon
    [[ -d "$pcsx2/.git" ]] || git clone -q --no-local --no-checkout "$repo/build-deps/pcsx2-2.8.2" "$pcsx2"
    git -C "$pcsx2" checkout -q -f --detach "$rev"; git -C "$pcsx2" clean -qfd
    for p in "$repo/tools/patches/pcsx2-gs-bridge-target.patch" "$repo/tools/patches/pcsx2-gs-bridge-linux.patch" \
             "$repo/tools/patches/android/pcsx2-android.patch"; do
        git -C "$pcsx2" apply "$p" || { echo "!! $p does not apply" >&2; exit 1; }
    done
    rm -rf "$out/bridge-src"; cp -R "$repo/tools/pcsx2-gs-bridge" "$out/bridge-src"
    (cd "$out/bridge-src" && patch -s -p1 < "$repo/tools/patches/android/bridge-android.diff" &&
        patch -s -p1 < "$repo/tools/patches/mobile/bridge-touch-draw.diff")
fi

# --------------------------------------------------------------------------------------------- 3. GS bridge
bridge_build="$out/build-pcsx2"
bridge_lib="$bridge_build/pcsx2-gs-bridge/librrv-pcsx2-gs-bridge.so"
if run_from bridge; then
    log "GS bridge (PCSX2, Vulkan) -> $bridge_build"
    cmake -S "$pcsx2" -B "$bridge_build" "${TC[@]}" -DCMAKE_SYSTEM_PROCESSOR=aarch64 \
        -DRRV_PCSX2_GS_BRIDGE_SOURCE_DIR="$out/bridge-src" \
        -DENABLE_QT_UI=OFF -DENABLE_TESTS=OFF -DENABLE_GSRUNNER=OFF -DUSE_OPENGL=OFF -DUSE_VULKAN=ON \
        -DX11_API=OFF -DWAYLAND_API=OFF -DUSE_BACKTRACE=OFF -DCMAKE_DISABLE_FIND_PACKAGE_FFMPEG=ON \
        -DFETCHCONTENT_SOURCE_DIR_SSE2NEON="$repo/build-deps/sse2neon-92f6de1" \
        -DSHADERC_INCLUDE_DIR="$deps/include" -DSHADERC_LIBRARY="$deps/lib/libshaderc_combined.a" \
        > "$out/bridge-configure.log" 2>&1 || { tail -40 "$out/bridge-configure.log"; exit 1; }
    cmake --build "$bridge_build" --target rrv-pcsx2-gs-bridge --parallel "$JOBS" -- -k 0 > "$out/bridge-build.log" 2>&1 ||
        { grep -E "error:|undefined reference" "$out/bridge-build.log" | sort -u | head -60; exit 1; }
    [[ -f "$bridge_lib" ]] || { echo "!! no $bridge_lib" >&2; exit 1; }
    # The product's resource package is described by a bridge manifest; Android writes its own (no build-tree
    # provenance: the APK signature covers what ships).
    python3 "$repo/tools/android/bridge_manifest.py" create --library "$bridge_lib" --pcsx2-source "$pcsx2" \
        --build-dir "$bridge_build" --output "$bridge_build/rrv-pcsx2-gs-bridge-manifest.json"
fi

# ---------------------------------------------------------------------------------------------- 4. product
product="$out/product"
generated="${MAGATAMA_GENERATED:-$(dirname "$repo")/generated-nocturne}"
if run_from product; then
    log "product (libmain.so) -> $product"
    [[ -f "$generated/register_functions.cpp" ]] || { echo "!! no recompiled game code at $generated" >&2; exit 1; }
    mkdir -p "$repo/generated/nocturne"; ln -sfn "$generated" "$repo/generated/nocturne/output"
    build_no="${MAGATAMA_ANDROID_BUILD_NO:-}"
    if [[ -z "$build_no" ]]; then
        counter="$HOME/magatama/android-build-number"; mkdir -p "$(dirname "$counter")"
        build_no=$(( $(cat "$counter" 2>/dev/null || echo 0) + 1 )); echo "$build_no" > "$counter"
    fi
    echo "$build_no" > "$out/build-number"
    # ARMv8.2 (Cortex-A75 / Snapdragon 845 and newer): inline LSE atomics instead of the outlined helpers the NDK's
    # armv8.0 default calls (Nova profile: ~5% of the VU1 thread). MAGATAMA_ANDROID_MARCH= (empty) builds for armv8.0.
    march="${MAGATAMA_ANDROID_MARCH--march=armv8.2-a}"
    cmake -S "$repo" -B "$product" "${TC[@]}" -DCMAKE_SYSTEM_PROCESSOR=aarch64 \
        -DBUILD_TESTING=OFF -DRRV_BUILD_PRODUCT=ON -DRRV_PRODUCT_OWNED_SOURCE=ON -DRRV_GS_PRODUCER_CONTROL=ON \
        -DMAGATAMA_GAME=ON -DMAGATAMA_ANDROID_DEPS="$deps" -DMAGATAMA_ANDROID_BUILD="$build_no" \
        -DCMAKE_C_FLAGS="$march" -DCMAKE_CXX_FLAGS="$march" \
        -DRRV_PCSX2_GS_BRIDGE_MANIFEST="$bridge_build/rrv-pcsx2-gs-bridge-manifest.json" \
        -DRRV_PCSX2_GS_BRIDGE_MANIFEST_VERIFIER="$repo/tools/android/bridge_manifest.py" \
        -DRRV_PCSX2_SOURCE_DIR="$repo/build-deps/pcsx2-2.8.2" \
        -DFETCHCONTENT_SOURCE_DIR_SSE2NEON="$repo/build-deps/sse2neon-92f6de1" \
        -DRRV_DISC_ZSTD_STATIC="$deps/lib/libzstd.a" -DRRV_DISC_ZSTD_INCLUDE_DIR="$deps/include" \
        > "$out/product-configure.log" 2>&1 || { grep -n "CMake Error" -A8 "$out/product-configure.log" | head -60; exit 1; }
    cmake --build "$product" --target Fukami --parallel "$JOBS" -- -k 0 > "$out/product-build.log" 2>&1 ||
        { grep -E "error:|undefined (reference|symbol)" "$out/product-build.log" | sed "s#$repo/##" | sort -u | head -80; exit 1; }
fi

# -------------------------------------------------------------------------------------------------- 5. APK
if run_from apk; then
    build_no="$(cat "$out/build-number")"
    bt="$(ls -d "$SDK"/build-tools/* | sort -V | tail -1)"; platform="$(ls -d "$SDK"/platforms/android-* | sort -V | tail -1)"
    jar="$platform/android.jar"
    apkdir="$out/apk"; rm -rf "$apkdir"; mkdir -p "$apkdir"/{lib/$ABI,assets/game,classes,res}
    log "APK (build $build_no, $(basename "$bt") build-tools, $(basename "$platform"))"
    # Native libraries.
    ndk_sysroot_lib="$(ls -d "$NDK"/toolchains/llvm/prebuilt/*/sysroot/usr/lib/aarch64-linux-android | head -1)"
    cp "$product/candidate-bin/libmain.so" "$deps/lib/libSDL3.so" "$deps/lib/libSDL2.so" "$bridge_lib" \
       "$ndk_sysroot_lib/libc++_shared.so" "$apkdir/lib/$ABI/"
    # Symbols stay in the build tree (crash logs give library + offset); the APK carries stripped copies.
    strip="$(ls "$NDK"/toolchains/llvm/prebuilt/*/bin/llvm-strip | head -1)"
    for so in "$apkdir/lib/$ABI/"*.so; do "$strip" --strip-unneeded "$so"; done
    # The resource package (as the product's build tree staged it) and the workload file, plus the extraction list.
    cp -RL "$product/.rrv-resource-packages/current" "$apkdir/assets/game/package"
    rm -f "$apkdir/assets/game/package/"*.so # the bridge loads from lib/ (an APK's only place for code)
    cp "$repo/config/magatama/bound-workload.txt" "$apkdir/assets/game/package/bound-workload.txt"
    (cd "$apkdir/assets/game/package" && { echo "# b$build_no-$(date +%s)"; find . -type f | sed 's#^\./##' | sort; }) \
        > "$apkdir/assets/game/package.list"
    # Java: SDL3's activity, compiled against the platform, dexed.
    sdljava="$deps/src/SDL3/android-project/app/src/main/java"
    javac -nowarn -source 11 -target 11 -encoding UTF-8 -cp "$jar" -d "$apkdir/classes" \
        $(find "$sdljava" "$repo/src/app/android" -name '*.java') 2>&1 | grep -v "^Note:\|warning" || true
    java -cp "$bt/lib/d8.jar" com.android.tools.r8.D8 --release --min-api $API --lib "$jar" --output "$apkdir" \
        $(find "$apkdir/classes" -name '*.class')
    # Resources (SDL's launcher icon for now) and the manifest.
    "$bt/aapt2" compile --dir "$deps/src/SDL3/android-project/app/src/main/res" -o "$apkdir/res.zip"
    sed -e "s/@PACKAGE@/dev.magatama.nocturne/" -e "s/@VERSION_CODE@/$build_no/" -e "s/@VERSION_NAME@/b$build_no/" \
        -e "s/@MIN_SDK@/$API/" -e "s/@TARGET_SDK@/$TARGET_SDK/" \
        "$repo/cmake/android/AndroidManifest.xml.in" > "$apkdir/AndroidManifest.xml"
    # MAGATAMA_ANDROID_DEBUGGABLE=1: a profiling build (simpleperf --app needs a debuggable app).
    [[ "${MAGATAMA_ANDROID_DEBUGGABLE:-0}" == 1 ]] && sed -i 's/<application/<application android:debuggable="true"/' "$apkdir/AndroidManifest.xml"
    "$bt/aapt2" link -o "$apkdir/unaligned.apk" -I "$jar" --manifest "$apkdir/AndroidManifest.xml" \
        -A "$apkdir/assets" "$apkdir/res.zip" --min-sdk-version $API --target-sdk-version $TARGET_SDK
    (cd "$apkdir" && zip -q -0 -r unaligned.apk lib && zip -q unaligned.apk classes.dex)
    # Uncompressed, 16 KB-aligned native libraries (extractNativeLibs=false; 16 KB-page devices), then sign.
    "$bt/zipalign" -f -P 16 4 "$apkdir/unaligned.apk" "$apkdir/aligned.apk"
    keystore="${MAGATAMA_ANDROID_KEYSTORE:-$HOME/magatama/android-debug.keystore}"
    [[ -f "$keystore" ]] || keytool -genkeypair -keystore "$keystore" -storepass android -keypass android \
        -alias magatama -keyalg RSA -keysize 2048 -validity 10000 -dname "CN=Magatama debug" -noprompt >/dev/null
    apk="$out/Magatama-android-b$build_no.apk"
    java -jar "$bt/lib/apksigner.jar" sign --ks "$keystore" --ks-pass pass:android --key-pass pass:android \
        --ks-key-alias magatama --out "$apk" "$apkdir/aligned.apk"
    ls -l "$apk"
    log "done: $apk"
fi
