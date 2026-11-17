#!/bin/zsh -f
# Magatama for iPhone (arm64, iOS 16+), unsigned IPA. Run by the job runner (ios=1) or by hand.
# Everything lands in build/ios/ (ignored). Stages resume: each finished stage leaves a stamp.
#   1. probe      toolchain facts (always)
#   2. deps       static iOS libraries the PCSX2 GS bridge and the host need -> build/ios/deps
set -u
repo="${0:A:h:h}"
# MAGATAMA_IOS_SIM=1 (or "sim" in the runner's request file): an iOS Simulator build in build/ios-sim, then a
# short unattended launch in the Simulator with the Mac's ~/magatama/disc (log and screenshot to
# ~/magatama/captures/ios-sim/).
sim="${MAGATAMA_IOS_SIM:-0}"
grep -qs sim "$HOME/magatama/jobs/.ios-build-request" && sim=1
if (( sim )); then sdkname=iphonesimulator; out="$repo/build/ios-sim"; else sdkname=iphoneos; out="$repo/build/ios"; fi
# MAGATAMA_IOS_LEGACY=1 (or "legacy" in the request file): iOS 15, A10 (ARMv8.0) baseline, 1x scale defaults,
# build/ios-legacy, Magatama-legacy-unsigned.ipa. For A11 and older (iPod touch 7, iPhone 7/8/X); A12 and newer use main.
legacy="${MAGATAMA_IOS_LEGACY:-0}"
grep -qs legacy "$HOME/magatama/jobs/.ios-build-request" && legacy=1
# Main: iOS 16, A12 (ARMv8.3) baseline: every device on current iOS (A12Z iPad Pros are the oldest) runs it.
minos=16.0; ios_cpu=apple-a12; ipa_name=Magatama-unsigned.ipa; legacy_pcsx2=""
if (( legacy && ! sim )); then
    minos=15.0; out="$repo/build/ios-legacy"; ipa_name=Magatama-legacy-unsigned.ipa
    legacy_pcsx2=1; ios_cpu=apple-a10
fi
cpu_flags=(-DCMAKE_C_FLAGS=-mcpu=$ios_cpu -DCMAKE_CXX_FLAGS=-mcpu=$ios_cpu -DCMAKE_OBJC_FLAGS=-mcpu=$ios_cpu
    -DCMAKE_OBJCXX_FLAGS=-mcpu=$ios_cpu)
(( sim )) && cpu_flags=()
mkdir -p "$out/src" "$out/deps" "$out/stamps"
deps="$out/deps"
print "== $(date)  repo=$repo"
print "== xcode: $(xcode-select -p 2>&1) / $(xcodebuild -version 2>&1 | tr '\n' ' ')"
xcodebuild -showsdks 2>&1 | grep -i "ios\|iphone" | head
sdk="$(xcrun --sdk $sdkname --show-sdk-path 2>&1)"; print "== $sdkname sdk: $sdk ($(xcrun --sdk $sdkname --show-sdk-version 2>&1))"
print "== cmake $(cmake --version 2>&1 | head -1) ninja=$(command -v ninja) git=$(command -v git)"
[[ -d "$sdk" ]] || { print "no iPhoneOS SDK (Xcode > Settings > Platforms > iOS)"; exit 1; }

ios_cmake=(-G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=$sdkname -DCMAKE_OSX_ARCHITECTURES=arm64
    -DCMAKE_OSX_DEPLOYMENT_TARGET=$minos "${cpu_flags[@]}" -DCMAKE_INSTALL_PREFIX="$deps" -DCMAKE_PREFIX_PATH="$deps"
    -DCMAKE_FIND_ROOT_PATH="$deps" -DCMAKE_INSTALL_LIBDIR=lib -DBUILD_SHARED_LIBS=OFF
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DCMAKE_XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED=NO)

fetch() { # name git-url tag
    local dir="$out/src/$1"
    [[ -d "$dir/.git" ]] && return 0
    rm -rf "$dir"; git clone -q --depth 1 --branch "$3" "$2" "$dir" || { print "clone failed: $2 $3"; return 1; }
}
dep() { # name srcdir-relative cmake-args...
    local name=$1 sub=$2; shift 2
    [[ -f "$out/stamps/$name" ]] && { print "== $name: done"; return 0; }
    print "== $name"
    rm -rf "$out/build-$name"
    cmake -S "$out/src/$name/$sub" -B "$out/build-$name" "${ios_cmake[@]}" "$@" > "$out/build-$name.log" 2>&1 &&
    cmake --build "$out/build-$name" --target install >> "$out/build-$name.log" 2>&1 &&
    touch "$out/stamps/$name" || { print "!! $name failed:"; tail -40 "$out/build-$name.log"; return 1; }
}
fail=0
fetch SDL2 https://github.com/libsdl-org/SDL.git release-2.32.10 && dep SDL2 . -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TEST=OFF || fail=1
# SDL3 without the camera (its AVFoundation/CoreMedia use bound to libswiftCoreMedia, absent on iOS 16).
grep -qs "SDL_CAMERA:BOOL=OFF" "$out/build-SDL3/CMakeCache.txt" || rm -f "$out/stamps/SDL3"
fetch SDL3 https://github.com/libsdl-org/SDL.git release-3.4.12 && dep SDL3 . -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TESTS=OFF -DSDL_TEST_LIBRARY=OFF -DSDL_CAMERA=OFF || fail=1
fetch zstd https://github.com/facebook/zstd.git v1.5.7 && dep zstd build/cmake -DZSTD_BUILD_SHARED=OFF -DZSTD_BUILD_PROGRAMS=OFF -DZSTD_BUILD_TESTS=OFF || fail=1
fetch lz4 https://github.com/lz4/lz4.git v1.10.0 && dep lz4 build/cmake -DLZ4_BUILD_CLI=OFF -DLZ4_BUILD_LEGACY_LZ4C=OFF || fail=1
fetch libpng https://github.com/pnggroup/libpng.git v1.6.58 && dep libpng . -DPNG_SHARED=OFF -DPNG_STATIC=ON -DPNG_TESTS=OFF -DPNG_TOOLS=OFF -DPNG_FRAMEWORK=OFF || fail=1
# CMake leaves CMAKE_SYSTEM_PROCESSOR empty for iOS; libjpeg-turbo picks its SIMD code from it.
fetch libjpeg-turbo https://github.com/libjpeg-turbo/libjpeg-turbo.git 3.2.0 &&
    sed -i '' 's/string(TOLOWER ${CMAKE_SYSTEM_PROCESSOR} CMAKE_SYSTEM_PROCESSOR_LC)/set(CMAKE_SYSTEM_PROCESSOR_LC aarch64)/' "$out/src/libjpeg-turbo/CMakeLists.txt" && dep libjpeg-turbo . -DCMAKE_SYSTEM_PROCESSOR=aarch64 -DENABLE_SHARED=OFF -DENABLE_STATIC=ON -DWITH_TURBOJPEG=OFF || fail=1
fetch libwebp https://github.com/webmproject/libwebp.git v1.6.0 && dep libwebp . -DWEBP_BUILD_ANIM_UTILS=OFF -DWEBP_BUILD_CWEBP=OFF -DWEBP_BUILD_DWEBP=OFF -DWEBP_BUILD_GIF2WEBP=OFF -DWEBP_BUILD_IMG2WEBP=OFF -DWEBP_BUILD_VWEBP=OFF -DWEBP_BUILD_WEBPINFO=OFF -DWEBP_BUILD_WEBPMUX=OFF -DWEBP_BUILD_EXTRAS=OFF || fail=1
fetch freetype https://github.com/freetype/freetype.git VER-2-13-3 && dep freetype . -DFT_DISABLE_HARFBUZZ=ON -DFT_DISABLE_BROTLI=ON -DFT_DISABLE_BZIP2=ON -DFT_DISABLE_PNG=ON || fail=1
fetch plutovg https://github.com/sammycage/plutovg.git v1.3.2 && dep plutovg . -DPLUTOVG_BUILD_EXAMPLES=OFF || fail=1
fetch plutosvg https://github.com/sammycage/plutosvg.git v0.0.7 && dep plutosvg . -DPLUTOSVG_ENABLE_FREETYPE=OFF -DPLUTOSVG_BUILD_EXAMPLES=OFF || fail=1
if [[ ! -d "$out/src/rapidyaml/.git" ]]; then
    rm -rf "$out/src/rapidyaml"; git clone -q --depth 1 --branch v0.12.1 --recurse-submodules --shallow-submodules https://github.com/biojppm/rapidyaml.git "$out/src/rapidyaml" || fail=1
fi
dep rapidyaml . -DRYML_BUILD_TESTS=OFF -DRYML_BUILD_BENCHMARKS=OFF || fail=1
fetch curl https://github.com/curl/curl.git curl-8_11_1 && dep curl . -DBUILD_CURL_EXE=OFF -DBUILD_TESTING=OFF -DCURL_ENABLE_SSL=OFF -DCURL_USE_LIBPSL=OFF -DUSE_LIBIDN2=OFF -DCURL_USE_LIBSSH2=OFF -DCURL_BROTLI=OFF -DCURL_ZSTD=OFF -DUSE_NGHTTP2=OFF -DCURL_DISABLE_LDAP=ON || fail=1
print "== deps: $(ls "$out/stamps" | tr '\n' ' ') fail=$fail"
ls "$deps/lib" 2>/dev/null | tr '\n' ' '; print
print "== iOS SDK headers: pcap=$(ls $sdk/usr/include/pcap.h 2>&1) curl=$(ls -d $sdk/usr/include/curl 2>&1)"
ls "$sdk/usr/lib" | grep -i "pcap\|curl"
(( fail )) && exit 1

# ---- 3. PCSX2 GS bridge for iOS: patched source (locked target patch + iOS patch), configure, build.
pcsx2="$out/pcsx2"
rev="$(awk '$0=="[pcsx2]"{p=1;next} p&&/^revision = /{gsub(/"/,"",$3);print $3;exit}' "$repo/config/dependencies.lock.toml")"
if [[ ! -d "$pcsx2/.git" ]]; then
    git clone -q --no-local --no-checkout "$repo/build-deps/pcsx2-2.8.2" "$pcsx2" || { print "!! pcsx2 clone failed"; exit 1; }
fi
# Fresh patched tree every run (the iOS patch is still changing); unchanged files keep their timestamps out of
# git's way only if identical, so expect a rebuild of what the patches touch.
git -C "$pcsx2" checkout -q -f --detach "$rev" && git -C "$pcsx2" clean -qfd &&
git -C "$pcsx2" apply "$repo/tools/patches/pcsx2-gs-bridge-target.patch" || { print "!! pcsx2 source prep failed"; exit 1; }
for p in "$repo"/tools/patches/ios/pcsx2-*.patch(N); do git -C "$pcsx2" apply "$p" || { print "!! $p"; exit 1; }; done
# iOS Metal libraries from the patched shaders (the macOS ones target air64-apple-macos).
metal="$out/metal"; mkdir -p "$metal/air" "$metal/cache"
metal_cc=(xcrun -sdk $sdkname metal)
sfx=""; (( sim )) && sfx="-simulator"
if ! "${metal_cc[@]}" -v > /dev/null 2>&1; then
    mounted=(/private/var/run/com.apple.security.cryptexd/mnt/com.apple.MobileAsset.MetalToolchain-*/Metal.xctoolchain/usr/metal/current/bin/metal(N))
    (( ${#mounted} )) && metal_cc=("${mounted[1]}" -isysroot "$sdk")
fi
print "== metal: ${metal_cc[*]}"
for lib std triple in default ios-metal2.0 air64-apple-ios13.0$sfx  Metal22 ios-metal2.2 air64-apple-ios13.0$sfx  Metal23 ios-metal2.3 air64-apple-ios14.0$sfx; do
    airs=()
    for s in cas convert present merge misc interlace tfx fxaa; do
        a="$metal/air/$lib-$s.air"
        "${metal_cc[@]}" -ffast-math -std=$std -target $triple -fmodules-cache-path="$metal/cache" \
            -o "$a" -c "$pcsx2/pcsx2/GS/Renderers/Metal/$s.metal" > "$metal/$lib-$s.log" 2>&1 ||
            { print "!! metal $lib $s"; tail -5 "$metal/$lib-$s.log"; exit 1; }
        airs+=("$a")
    done
    # The link step needs the target too, or Xcode 27 marks the library for its own iOS (refused by iOS 16).
    "${metal_cc[@]}" -target $triple -o "$metal/$lib.metallib" "${airs[@]}" > "$metal/$lib-link.log" 2>&1 ||
        { print "!! metallib $lib"; tail -5 "$metal/$lib-link.log"; exit 1; }
done
ls -la "$metal"/*.metallib
# Record what deployment target the libraries carry (iOS refuses a newer one than it runs).
for m in "$metal"/*.metallib; do print "$m: $(xcrun -sdk $sdkname metal-objdump --macho --private-headers "$m" 2>/dev/null | grep -iE "platform|minos|version" | head -4 | tr '\n' ' ')"; done
# The bridge sources with the iOS surface check (UIView), outside the repository's provenance-checked tree.
rm -rf "$out/bridge-src"; cp -R "$repo/tools/pcsx2-gs-bridge" "$out/bridge-src" &&
(cd "$out/bridge-src" && patch -s -p1 < "$repo/tools/patches/ios/bridge-metal-surface-ios.diff" &&
    patch -s -p1 < "$repo/tools/patches/ios/bridge-resources-ios.diff" &&
    patch -s -p1 < "$repo/tools/patches/mobile/bridge-touch-draw.diff") ||
    { print "!! bridge source prep failed"; exit 1; }
print "== pcsx2 configure (metal resources: $metal)"
cmake -S "$pcsx2" -B "$out/build-pcsx2" "${ios_cmake[@]}" -DCMAKE_SYSTEM_PROCESSOR=aarch64 \
    -DRRV_PCSX2_GS_BRIDGE_SOURCE_DIR="$out/bridge-src" -DRRV_PCSX2_GS_METAL_RESOURCE_DIR="$metal" \
    ${cpu_flags:+-DRRV_IOS_CPU=$ios_cpu} \
    "-DCMAKE_SHARED_LINKER_FLAGS=-framework CoreMedia -framework AVFoundation -framework CoreVideo" \
    -DENABLE_QT_UI=OFF -DENABLE_TESTS=OFF -DUSE_VULKAN=OFF -DUSE_OPENGL=OFF -DCMAKE_DISABLE_FIND_PACKAGE_FFMPEG=ON \
    -DFETCHCONTENT_SOURCE_DIR_SSE2NEON="$repo/build-deps/sse2neon-92f6de1" > "$out/build-pcsx2-configure.log" 2>&1
st=$?; grep -n "CMake Error\|Could NOT\|error:" -A3 "$out/build-pcsx2-configure.log" | head -60
(( st )) && { print "!! pcsx2 configure failed"; tail -20 "$out/build-pcsx2-configure.log"; exit 1; }
print "== pcsx2 build (bridge)"
cmake --build "$out/build-pcsx2" --target rrv-pcsx2-gs-bridge -- -k 0 > "$out/build-pcsx2.log" 2>&1
st=$?; grep -n "error:" "$out/build-pcsx2.log" | sort -t: -k3 -u | head -80
(( st )) && exit $st
bridge_lib="$out/build-pcsx2/pcsx2-gs-bridge/librrv-pcsx2-gs-bridge.dylib"
install_name_tool -id @rpath/librrv-pcsx2-gs-bridge.dylib "$bridge_lib" 2>/dev/null
print "== bridge symbols bound to Swift overlays:"; nm -m "$bridge_lib" | grep -i "from libswift" | head -20
# Its Swift overlay dependencies (@rpath/libswift*.dylib) live in the OS on iOS 16+.
otool -l "$bridge_lib" | grep -q "path /usr/lib/swift " || install_name_tool -add_rpath /usr/lib/swift "$bridge_lib"

# Build number for the app (CFBundleVersion, shown in the log) and the archived file names: one counter for every
# iOS build variant, ~/magatama/ios-build-number. Magatama.tipa of 2026-11-03 19:44 was build 1.
num_file="$HOME/magatama/ios-build-number"
# "number=N" in the request file (or MAGATAMA_IOS_BUILD_NO) builds a variant under an existing number (e.g. the
# legacy twin of iPhone build N) without advancing the counter.
fixed_no="${MAGATAMA_IOS_BUILD_NO:-$(grep -os 'number=[0-9]*' "$HOME/magatama/jobs/.ios-build-request" | cut -d= -f2)}"
if [[ -n "$fixed_no" ]]; then build_no=$fixed_no
else build_no=$(( $(cat "$num_file" 2>/dev/null || print 1) + 1 )); print $build_no > "$num_file"; fi
print "== iOS build number $build_no"

# ---- 4. The game (same graph as scripts/magatama_build.sh, cross-compiled for iOS). The configure-time
# provenance checks run against the macOS bridge the Mac build uses; the app bundle then carries the iOS bridge.
live_manifest="$repo/build/live-settings-bridge/rrv-pcsx2-gs-bridge-manifest.json"
export MAGATAMA_BRIDGE_ROOT="$repo/build/live-settings-bridge-source"
product="$out/product"
# The launcher (src/launcher/ios, SwiftUI): one object, linked into the app with the Swift runtime from the OS.
print "== launcher (Swift)"
swift_target="arm64-apple-ios$minos"; (( sim )) && swift_target="arm64-apple-ios$minos-simulator"
launcher_obj="$out/magatama-launcher.o"
xcrun --sdk $sdkname swiftc -parse-as-library -emit-object -O -target "$swift_target" -sdk "$sdk" \
    -module-name MagatamaLauncher -import-objc-header "$repo/src/app/magatama_disc_import.h" \
    "$repo/src/launcher/ios/MagatamaLauncher.swift" -o "$launcher_obj" > "$out/launcher-build.log" 2>&1 ||
    { print "!! launcher (Swift) failed"; tail -30 "$out/launcher-build.log"; exit 1; }
swift_lib="$(dirname "$(xcrun --sdk $sdkname -f swiftc)")/../lib/swift/$sdkname"
swift_link="-L$sdk/usr/lib/swift -L$swift_lib -Wl,-rpath,/usr/lib/swift"
print "== product configure"
# CMake leaves CMAKE_SYSTEM_PROCESSOR empty for iOS; the runtime picks sse2neon (ARM64) from it.
[[ -f "$product/CMakeCache.txt" ]] && ! grep -q "^CMAKE_SYSTEM_PROCESSOR:.*=arm64" "$product/CMakeCache.txt" && rm -rf "$product"
cmake -S "$repo" -B "$product" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_SYSTEM_PROCESSOR=arm64 -DCMAKE_OSX_SYSROOT=$sdkname \
    -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=$minos "${cpu_flags[@]}" \
    -DMAGATAMA_IOS_MIN_OS=$minos -DMAGATAMA_IOS_LEGACY=$legacy -DMAGATAMA_IOS_BUILD=$build_no \
    -DBUILD_TESTING=OFF -DRRV_BUILD_PRODUCT=ON -DRRV_PRODUCT_OWNED_SOURCE=ON -DRRV_GS_PRODUCER_CONTROL=ON \
    -DMAGATAMA_GAME=ON -DMAGATAMA_IOS_DEPS="$deps" \
    -DMAGATAMA_IOS_LAUNCHER_OBJECT="$launcher_obj" -DMAGATAMA_IOS_SWIFT_LINK="$swift_link" \
    -DRRV_PCSX2_GS_BRIDGE_MANIFEST="$live_manifest" \
    -DRRV_PCSX2_SOURCE_DIR="$HOME/fukami/build-deps/pcsx2-2.8.2" \
    -DFETCHCONTENT_SOURCE_DIR_SSE2NEON="$repo/build-deps/sse2neon-92f6de1" \
    -DRRV_DISC_ZSTD_STATIC="$deps/lib/libzstd.a" -DRRV_DISC_ZSTD_INCLUDE_DIR="$deps/include" \
    > "$out/product-configure.log" 2>&1
st=$?; grep -n "CMake Error" -A6 "$out/product-configure.log" | head -60
(( st )) && { print "!! product configure failed"; tail -25 "$out/product-configure.log"; exit 1; }
print "== product build"
cmake --build "$product" --target Fukami -- -k 0 > "$out/product-build.log" 2>&1
st=$?
grep -E "(error|fatal error):" "$out/product-build.log" | sed -E "s#$repo/##" | sort -u | head -60
awk '/Undefined symbols/{f=1} f' "$out/product-build.log" | grep '^  "' | sort -u | head -40
(( st )) && { print "!! product build failed"; tail -5 "$out/product-build.log"; exit 1; }

# ---- 5. App bundle and unsigned IPA.
app="$product/candidate-bin/Magatama.app"
[[ -d "$app" ]] || { print "!! no $app"; exit 1; }
# iOS 27 kills an app at launch when it was linked against the iOS 27 SDK without the UIScene life cycle, which
# SDL2's UIKit app delegate does not have ("UIScene life cycle is required for apps built with this SDK"). The
# check reads the SDK recorded in the executable, so record iOS 26.0 (what an Xcode 26 build would carry) until the
# launcher adopts scenes. Re-signed ad hoc after the edit; the installer signs it for real.
xcrun vtool -set-build-version ios $minos 26.0 -replace -output "$app/Magatama" "$app/Magatama" &&
    codesign -f -s - "$app/Magatama" || { print "!! could not set the recorded SDK"; exit 1; }
# The package as game/package (no hidden names in an app bundle; see rrv_resource_package.cpp).
game="$app/game"; rm -rf "$game"; mkdir -p "$game"
cp -RL "$product/.rrv-resource-packages/current" "$game/package" || { print "!! no package"; exit 1; }
chmod -R u+w "$game"
cp "$bridge_lib" "$game/package/librrv-pcsx2-gs-bridge.dylib"
mkdir -p "$app/Frameworks"; cp "$bridge_lib" "$app/Frameworks/librrv-pcsx2-gs-bridge.dylib"
for m in default Metal22 Metal23; do
    cp "$out/metal/$m.metallib" "$game/package/resources/$m.metallib"
done
cp "$repo/config/magatama/bound-workload.txt" "$app/bound-workload.txt"
# RULE (CONTRIBUTING.md, "Hard rules"): landscape only on iPhone and iPad. Refuse to package anything else.
for key in UISupportedInterfaceOrientations "UISupportedInterfaceOrientations~ipad"; do
    orientations="$(plutil -extract "$key" xml1 -o - "$app/Info.plist" 2>/dev/null)"
    if [[ -z "$orientations" ]] || [[ "$orientations" == *Portrait* ]] ||
        [[ "$orientations" != *UIInterfaceOrientationLandscapeLeft* ]] ||
        [[ "$orientations" != *UIInterfaceOrientationLandscapeRight* ]]; then
        print "!! $key must list exactly the two landscape orientations (Magatama is landscape only on iOS)"
        exit 1
    fi
done
ipa="$out/$ipa_name"; rm -rf "$out/ipa" "$ipa"; mkdir -p "$out/ipa/Payload"
cp -R "$app" "$out/ipa/Payload/"
(cd "$out/ipa" && /usr/bin/zip -qry "$ipa" Payload) || { print "!! zip failed"; exit 1; }
ls -la "$ipa"; du -sh "$app"
print "== bundle check"
plutil -p "$app/Info.plist" | head -8
for m in "$app/Magatama" "$app/Frameworks/librrv-pcsx2-gs-bridge.dylib"; do
    print "$m"; otool -l "$m" | grep -A4 LC_BUILD_VERSION | grep -E "platform|minos|sdk" | head -3
    otool -L "$m" | head -40; otool -l "$m" | grep -A2 LC_RPATH | grep " path"
done
if (( ! sim )); then
    # Archived as ~/magatama/ios-builds/Magatama[-legacy]-b<N>.ipa and .tipa (TrollStore opens .tipa directly).
    arch="$HOME/magatama/ios-builds"; mkdir -p "$arch"
    base="${ipa_name%-unsigned.ipa}-b$build_no"
    cp "$ipa" "$arch/$base.ipa"; cp "$ipa" "$arch/$base.tipa"; chmod 644 "$arch/$base.ipa" "$arch/$base.tipa"
    print "== done: ~/magatama/ios-builds/$base.ipa and $base.tipa"
    exit 0
fi

# ---- 6. Simulator smoke test (no input; ~90 s from launch).
cap="$HOME/magatama/captures/ios-sim"; mkdir -p "$cap"
dev="$(xcrun simctl list devices available | grep -E "iPhone 1[5-9]|iPhone 2" | head -1 | grep -oE "[0-9A-F-]{36}")"
if [[ -z "$dev" ]]; then
    rt="$(xcrun simctl list runtimes available | grep -i "^iOS" | tail -1 | grep -oE "com\.apple\.CoreSimulator\.SimRuntime\.[A-Za-z0-9-]+")"
    ty="$(xcrun simctl list devicetypes | grep -E "^iPhone 1[6-9] |^iPhone 2" | head -1 | grep -oE "com\.apple\.CoreSimulator\.SimDeviceType\.[A-Za-z0-9-]+")"
    print "== no iPhone simulator; runtime=$rt type=$ty"
    xcrun simctl list runtimes 2>&1 | head -10
    [[ -n "$rt" && -n "$ty" ]] && dev="$(xcrun simctl create "Magatama iPhone" "$ty" "$rt" 2>&1 | tail -1)"
fi
print "== simulator device: $dev"
[[ "$dev" =~ '^[0-9A-F-]{36}$' ]] || { print "!! no simulator device (install an iOS Simulator runtime in Xcode > Settings > Components)"; exit 1; }
xcrun simctl boot "$dev" 2>/dev/null; xcrun simctl bootstatus "$dev" -b > /dev/null 2>&1
xcrun simctl uninstall "$dev" io.github.bittahbandit1.magatama 2>/dev/null
xcrun simctl install "$dev" "$app" || { print "!! install failed"; exit 1; }
data="$(xcrun simctl get_app_container "$dev" io.github.bittahbandit1.magatama data)"
mkdir -p "$data/Documents"
rm -rf "$data/Documents/disc"; ln -s "$HOME/magatama/disc" "$data/Documents/disc"
mkdir -p "$data/Documents/memcard"; cp -R "$HOME/magatama/jobs/euro-20261026/memcard-before/"* "$data/Documents/memcard/" 2>/dev/null
xcrun simctl launch "$dev" io.github.bittahbandit1.magatama > "$cap/launch.txt" 2>&1
for s in 20 45 90; do
    sleep $(( s - ${prev:-0} )); prev=$s
    xcrun simctl io "$dev" screenshot "$cap/screen-$s.png" > /dev/null 2>&1
done
cp "$(ls -t "$data/Documents/logs/"*.log | head -1)" "$cap/run.log" 2>/dev/null
xcrun simctl terminate "$dev" io.github.bittahbandit1.magatama 2>/dev/null
log show --last 3m --predicate 'process == "Magatama"' --style compact 2>/dev/null | tail -60 > "$cap/system.txt"
ls -la "$cap"; tail -30 "$cap/run.log" 2>/dev/null
exit 0
