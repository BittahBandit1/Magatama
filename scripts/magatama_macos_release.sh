#!/bin/zsh -f
# Build the macOS release: "Magatama Installer.app", which contains no game code. The user drops their own disc
# image on it and it builds Magatama.app on their Mac (src/launcher/macos-installer/magatama_install.py).
#
#   scripts/magatama_macos_release.sh [version]          (default: the VERSION file)
#
# Needs what scripts/magatama_build.sh needs (the PCSX2 GS bridge, a generated/nocturne layout from your own ELF)
# plus Xcode. Everything lands in build/macos-release/ (ignored):
#   product/        a product build without link-time optimisation (its objects must link with any newer Apple clang)
#   kit/            the build kit: prebuilt runtime, headers, recipe, recompiler, generators, Magatama.app template
#   Magatama Installer.app, Magatama-<version>-macOS-Installer.zip
set -eu
repo="${0:A:h:h}"
version="${1:-$(cat "$repo/VERSION")}"
jobs="${MAGATAMA_JOBS:-8}"
out="$repo/build/macos-release"
build="$out/product"
target=14.0     # the oldest macOS the release runs on (checked against every bundled library below)
export PATH="/opt/homebrew/bin:$PATH"
mkdir -p "$out"

# ---- Libraries for the oldest supported macOS. Homebrew's are built for the Mac they were installed on (macOS
# refuses to open an app that carries a library newer than itself), so the release builds its own: static
# libraries for the PCSX2 GS bridge, and SDL2 (a dylib, as the product expects) for the game.
deps="$out/deps"; src="$out/deps-src"; stamps="$out/deps-stamps"
mkdir -p "$deps" "$src" "$stamps"
mac_cmake=(-G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=$target
    -DCMAKE_INSTALL_PREFIX="$deps" -DCMAKE_PREFIX_PATH="$deps" -DCMAKE_INSTALL_LIBDIR=lib -DBUILD_SHARED_LIBS=OFF
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON "-DCMAKE_IGNORE_PREFIX_PATH=/opt/homebrew;/usr/local" -DCMAKE_FIND_FRAMEWORK=LAST)
fetch() { # name git-url tag
    [[ -d "$src/$1/.git" ]] && return 0
    rm -rf "$src/$1"; git clone -q --depth 1 --branch "$3" --recurse-submodules --shallow-submodules "$2" "$src/$1"
}
dep() { # name srcdir-relative cmake-args...
    local name=$1 sub=$2; shift 2
    [[ -f "$stamps/$name" ]] && return 0
    print "   $name"
    rm -rf "$out/deps-build/$name"
    { cmake -S "$src/$name/$sub" -B "$out/deps-build/$name" "${mac_cmake[@]}" "$@" &&
      cmake --build "$out/deps-build/$name" --target install; } > "$out/deps-build-$name.log" 2>&1 &&
    touch "$stamps/$name" || { tail -30 "$out/deps-build-$name.log"; return 1; }
}
print "== libraries (macOS $target)"
fetch SDL3 https://github.com/libsdl-org/SDL.git release-3.4.12 && dep SDL3 . -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TESTS=OFF -DSDL_TEST_LIBRARY=OFF
fetch zstd https://github.com/facebook/zstd.git v1.5.7 && dep zstd build/cmake -DZSTD_BUILD_SHARED=OFF -DZSTD_BUILD_PROGRAMS=OFF -DZSTD_BUILD_TESTS=OFF
fetch lz4 https://github.com/lz4/lz4.git v1.10.0 && dep lz4 build/cmake -DLZ4_BUILD_CLI=OFF -DLZ4_BUILD_LEGACY_LZ4C=OFF
fetch libpng https://github.com/pnggroup/libpng.git v1.6.58 && dep libpng . -DPNG_SHARED=OFF -DPNG_STATIC=ON -DPNG_TESTS=OFF -DPNG_TOOLS=OFF -DPNG_FRAMEWORK=OFF
fetch libjpeg-turbo https://github.com/libjpeg-turbo/libjpeg-turbo.git 3.2.0 && dep libjpeg-turbo . -DENABLE_SHARED=OFF -DENABLE_STATIC=ON -DWITH_TURBOJPEG=OFF
fetch libwebp https://github.com/webmproject/libwebp.git v1.6.0 && dep libwebp . -DWEBP_BUILD_ANIM_UTILS=OFF -DWEBP_BUILD_CWEBP=OFF -DWEBP_BUILD_DWEBP=OFF -DWEBP_BUILD_GIF2WEBP=OFF -DWEBP_BUILD_IMG2WEBP=OFF -DWEBP_BUILD_VWEBP=OFF -DWEBP_BUILD_WEBPINFO=OFF -DWEBP_BUILD_WEBPMUX=OFF -DWEBP_BUILD_EXTRAS=OFF
# PCSX2 links libwebp alone; its encoder needs libsharpyuv, a separate archive since libwebp 1.3.
[[ -f "$stamps/webp-merged" ]] || { libtool -static -o "$deps/lib/libwebp.a.merged" "$deps/lib/libwebp.a" "$deps/lib/libsharpyuv.a" &&
    mv "$deps/lib/libwebp.a.merged" "$deps/lib/libwebp.a" && touch "$stamps/webp-merged"; }
fetch freetype https://github.com/freetype/freetype.git VER-2-13-3 && dep freetype . -DFT_DISABLE_HARFBUZZ=ON -DFT_DISABLE_BROTLI=ON -DFT_DISABLE_BZIP2=ON -DFT_DISABLE_PNG=ON
fetch plutovg https://github.com/sammycage/plutovg.git v1.3.2 && dep plutovg . -DPLUTOVG_BUILD_EXAMPLES=OFF
fetch plutosvg https://github.com/sammycage/plutosvg.git v0.0.7 && dep plutosvg . -DPLUTOSVG_ENABLE_FREETYPE=OFF -DPLUTOSVG_BUILD_EXAMPLES=OFF
fetch rapidyaml https://github.com/biojppm/rapidyaml.git v0.12.1 && dep rapidyaml . -DRYML_BUILD_TESTS=OFF -DRYML_BUILD_BENCHMARKS=OFF
# SDL2 for the game, as a dylib in its own prefix (lib/libSDL2-2.0.0.dylib, include/SDL2).
sdl2="$out/sdl2"
fetch SDL2 https://github.com/libsdl-org/SDL.git release-2.32.10
if [[ ! -f "$stamps/SDL2" ]]; then
    print "   SDL2"
    { cmake -S "$src/SDL2" -B "$out/deps-build/SDL2" "${mac_cmake[@]}" -DCMAKE_INSTALL_PREFIX="$sdl2" \
        -DBUILD_SHARED_LIBS=ON -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TEST=OFF &&
      cmake --build "$out/deps-build/SDL2" --target install; } > "$out/deps-build-SDL2.log" 2>&1 &&
    install_name_tool -id @rpath/libSDL2-2.0.0.dylib "$sdl2/lib/libSDL2-2.0.0.dylib" &&
    touch "$stamps/SDL2" || { tail -30 "$out/deps-build-SDL2.log"; exit 1; }
fi

print "== bridge (macOS $target, the libraries above)"
# The bridge sources and builder of the live-settings checkout; the product's provenance checks read them there.
export MAGATAMA_BRIDGE_ROOT="$repo/build/live-settings-bridge-source"
bridge="$out/bridge"
bridge_manifest="$bridge/rrv-pcsx2-gs-bridge-manifest.json"
# Rebuilt when the locked bridge patch changed (the manifest records the patch it was built with).
locked_patch="$(awk '$0=="[[pcsx2_bridge_patches]]"{p=1;next} p&&/^sha256 = /{gsub(/"/,"",$3);print $3;exit}' "$MAGATAMA_BRIDGE_ROOT/config/dependencies.lock.toml")"
if [[ -f "$bridge_manifest" ]] && ! grep -q "$locked_patch" "$bridge_manifest"; then
    print "   the bridge patch changed: rebuilding the bridge"; rm -f "$bridge_manifest"
fi
if [[ ! -f "$bridge_manifest" ]]; then
    # The Metal libraries of this patch, as scripts/magatama_build_live_bridge.py names them (run
    # scripts/magatama_build.sh once after a patch change to make them).
    metal="$(cd "$repo/build/live-settings-bridge" && pwd -P)/../metal-resources-${locked_patch:0:12}"
    [[ -f "$metal/metal-resources-manifest.toml" ]] || { print -u2 "no Metal libraries for this patch: $metal"; exit 1; }
    rm -rf "$bridge" "$bridge"-source-*(N)
    PCSX2_SOURCE_DIR="${MAGATAMA_PCSX2_SOURCE_DIR:-$repo/build-deps/pcsx2-2.8.2}" PCSX2_GS_BRIDGE_BUILD_DIR="$bridge" \
    PCSX2_METAL_RESOURCE_DIR="$metal" PCSX2_METAL_RESOURCE_MANIFEST="$metal/metal-resources-manifest.toml" \
    FUKAMI_MACOS_TARGET=$target CMAKE_BUILD_PARALLEL_LEVEL=$jobs \
    "$MAGATAMA_BRIDGE_ROOT/scripts/build_pcsx2_gs_bridge.sh" -G Ninja -DCMAKE_BUILD_TYPE=Release -DENABLE_QT_UI=OFF \
        -DENABLE_TESTS=OFF -DUSE_VULKAN=OFF -DCMAKE_DISABLE_FIND_PACKAGE_FFMPEG=ON -DCMAKE_PREFIX_PATH="$deps" \
        "-DCMAKE_IGNORE_PREFIX_PATH=/opt/homebrew;/usr/local" -DCMAKE_FIND_FRAMEWORK=LAST \
        -DFETCHCONTENT_SOURCE_DIR_SSE2NEON="$repo/build-deps/sse2neon-92f6de1" > "$out/bridge-build.log" 2>&1 ||
        { grep -E "error|Error" "$out/bridge-build.log" | head -30; tail -20 "$out/bridge-build.log"; exit 1; }
fi
print "   $(otool -L "$bridge/pcsx2-gs-bridge/librrv-pcsx2-gs-bridge.dylib" | grep -cE '/opt/homebrew|/usr/local') Homebrew libraries linked"
[[ -f "$repo/generated/nocturne/source-manifest.json" ]] || { print -u2 "run scripts/magatama_generate.py first"; exit 1; }

print "== product (no LTO, macOS $target)"
cmake -S "$repo" -B "$build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF -DCMAKE_OSX_DEPLOYMENT_TARGET=$target -DRRV_NO_IPO=ON \
    -DRRV_BUILD_PRODUCT=ON -DRRV_PRODUCT_OWNED_SOURCE=ON -DRRV_GS_PRODUCER_CONTROL=ON -DMAGATAMA_GAME=ON \
    -DRRV_PCSX2_GS_BRIDGE_MANIFEST="$bridge_manifest" -DMAGATAMA_MACOS_SDL_PREFIX="$sdl2" \
    -DRRV_PCSX2_GS_BRIDGE_MANIFEST_VERIFIER="$MAGATAMA_BRIDGE_ROOT/scripts/pcsx2_bridge_manifest.py" \
    -DRRV_DISC_ZSTD_STATIC="$deps/lib/libzstd.a" -DRRV_DISC_ZSTD_INCLUDE_DIR="$deps/include" \
    -DRRV_PCSX2_SOURCE_DIR="${MAGATAMA_PCSX2_SOURCE_DIR:-$repo/build-deps/pcsx2-2.8.2}" \
    -DFETCHCONTENT_SOURCE_DIR_SSE2NEON="$repo/build-deps/sse2neon-92f6de1" > "$out/product-configure.log"
cmake --build "$build" --target Fukami --parallel "$jobs" > "$out/product-build.log" 2>&1 ||
    { tail -20 "$out/product-build.log"; exit 1; }

print "== kit"
kit="$out/kit"
python3 "$repo/scripts/magatama_macos_kit.py" --build "$build" --out "$kit"
mkdir -p "$kit/tools/vu-aot" "$kit/config" "$kit/app"
cp "$repo/tools/vu-aot/vu_aot_gen.py" "$kit/tools/vu-aot/"
cp "$repo/config/magatama/nocturne-recomp.toml.in" "$repo/config/magatama/nocturne-functions.csv" "$kit/config/"

print "== recompiler"
cmake -S "$repo" -B "$out/recompiler" -DRRV_BUILD_RECOMPILER=ON -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=$target > "$out/recompiler-configure.log"
cmake --build "$out/recompiler" --target ps2_recomp --parallel "$jobs" > "$out/recompiler-build.log" 2>&1 ||
    { tail -20 "$out/recompiler-build.log"; exit 1; }
cp "$(find "$out/recompiler" -name ps2_recomp -type f -perm -u+x | head -1)" "$kit/tools/ps2_recomp"
MAGATAMA_RECOMPILER_BUILD="$out/recompiler" python3 "$repo/scripts/magatama_generate_bezier.py" \
    --build-generator "$kit/tools/nocturne-effects-generate" --target $target
pcsx2="${MAGATAMA_PCSX2_SOURCE_DIR:-$repo/build-deps/pcsx2-2.8.2}"
zstd="$deps/lib/libzstd.a"
xcrun clang++ -std=c++20 -O2 -mmacosx-version-min=$target -I"$repo/src/app" -I"$pcsx2/3rdparty/libchdr/include" \
    "$repo/src/app/magatama_disc_import.cpp" "$repo/src/app/magatama_disc_import_tool.cpp" \
    "$build/librrv-disc-extract.a" "$build/librrv-disc-chdr.a" "$zstd" -lz -o "$kit/tools/magatama-disc-import"

print "== Magatama.app template (the launcher and the runtime; the installer adds the game)"
app="$kit/app/Magatama.app"
mkdir -p "$app/Contents/MacOS" "$app/Contents/Frameworks" "$app/Contents/Resources/workload" "$app/Contents/Resources/game"
xcrun clang++ -std=c++20 -O2 -fobjc-arc -target arm64-apple-macos$target -I"$repo/src/app" \
    -c "$repo/src/launcher/macos/MagatamaSettingsBridge.mm" -o "$out/settings-bridge.o" || exit 1
xcrun swiftc -parse-as-library -O -swift-version 5 -target arm64-apple-macos$target \
    -import-objc-header "$repo/src/launcher/macos/MagatamaSettingsBridge.h" \
    "$repo/src/launcher/macos/MagatamaLauncherMac.swift" "$out/settings-bridge.o" "$build/librrv_fukami_settings.a" -lc++ -o "$app/Contents/MacOS/Magatama"
cp "$kit/tools/magatama-disc-import" "$app/Contents/MacOS/"
cp "$repo/config/magatama/bound-workload.txt" "$app/Contents/Resources/workload/"
touch "$app/Contents/Resources/game/.rrv-resource-package.lock"
cp -RL "$build/.rrv-resource-packages" "$app/Contents/Resources/game/"
cp "$repo/LICENSE" "$app/Contents/Resources/LICENSE.txt"
# The icon and the wordmark (assets/branding): Magatama.icns from the 1024 master, for both apps.
icons="$out/Magatama.iconset"; rm -rf "$icons"; mkdir -p "$icons"
for px in 16 32 128 256 512; do
    sips -z $px $px "$repo/assets/branding/magatama-icon-1024.png" --out "$icons/icon_${px}x${px}.png" > /dev/null
    sips -z $((px * 2)) $((px * 2)) "$repo/assets/branding/magatama-icon-1024.png" --out "$icons/icon_${px}x${px}@2x.png" > /dev/null
done
iconutil -c icns "$icons" -o "$out/Magatama.icns"; rm -rf "$icons"
cp "$out/Magatama.icns" "$app/Contents/Resources/Magatama.icns"
cp "$repo/assets/branding/magatama-wordmark.png" "$app/Contents/Resources/MagatamaWordmark.png"
python3 - "$repo" "$app" "$version" "$target" "$sdl2" <<'PY'
import importlib.util, plistlib, shutil, sys
from pathlib import Path
repo, app, version, target, sdl2 = Path(sys.argv[1]), Path(sys.argv[2]), sys.argv[3], sys.argv[4], Path(sys.argv[5])
spec = importlib.util.spec_from_file_location("pkg", repo / "scripts/package_fukami_app.py")
pkg = importlib.util.module_from_spec(spec); spec.loader.exec_module(pkg)
fw = app / "Contents/Frameworks"
sdl = sdl2 / "lib/libSDL2-2.0.0.dylib"
shutil.copy2(sdl, fw / sdl.name)
pkg.run("install_name_tool", "-id", "@rpath/libSDL2-2.0.0.dylib", str(fw / sdl.name))
bridge = app / "Contents/Resources/game/.rrv-resource-packages/current/librrv-pcsx2-gs-bridge.dylib"
extra = pkg.bundle_closure(bridge, fw)
version_key = lambda v: tuple(int(x) for x in v.split("."))
found = {p.name: pkg.min_macos(p) for p in [bridge, *fw.iterdir()]}
too_new = {name: v for name, v in found.items() if version_key(v) > version_key(target)}
if too_new:
    sys.exit(f"libraries built for a newer macOS than {target}: {too_new}")
minimum = target
(app / "Contents/Resources/LICENSE-NOTICES.txt").write_text(pkg.notices([sdl.name] + extra))
plistlib.dump({
    "CFBundleExecutable": "Magatama", "CFBundleIdentifier": "io.github.bittahbandit1.magatama.mac", "CFBundleIconFile": "Magatama",
    "CFBundleName": "Magatama", "CFBundleDisplayName": "Magatama", "CFBundlePackageType": "APPL",
    "CFBundleShortVersionString": version, "CFBundleVersion": version, "LSMinimumSystemVersion": minimum,
    "NSHighResolutionCapable": True, "NSPrincipalClass": "NSApplication",
    "LSApplicationCategoryType": "public.app-category.role-playing-games",
}, open(app / "Contents/Info.plist", "wb"))
print("minimum macOS", minimum, "| bundled:", ", ".join([sdl.name] + extra))
(app.parent / "minimum-macos.txt").write_text(minimum + "\n")
PY
minimum="$(cat "$kit/app/minimum-macos.txt")"; rm "$kit/app/minimum-macos.txt"

print "== installer"
inst="$out/Magatama Installer.app"
rm -rf "$inst"; mkdir -p "$inst/Contents/MacOS" "$inst/Contents/Resources"
xcrun swiftc -parse-as-library -O -swift-version 5 -target arm64-apple-macos$target \
    "$repo/src/launcher/macos-installer/MagatamaInstaller.swift" -o "$inst/Contents/MacOS/MagatamaInstaller"
cp -R "$kit" "$inst/Contents/Resources/kit"
cp "$repo/src/launcher/macos-installer/magatama_install.py" "$inst/Contents/Resources/"
cp "$repo/LICENSE" "$inst/Contents/Resources/LICENSE.txt"
cp "$out/Magatama.icns" "$inst/Contents/Resources/Magatama.icns"
cp "$repo/assets/branding/magatama-wordmark.png" "$inst/Contents/Resources/MagatamaWordmark.png"
cat > "$inst/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>CFBundleExecutable</key><string>MagatamaInstaller</string>
	<key>CFBundleIconFile</key><string>Magatama</string>
	<key>CFBundleIdentifier</key><string>io.github.bittahbandit1.magatama.installer</string>
	<key>CFBundleName</key><string>Magatama Installer</string>
	<key>CFBundlePackageType</key><string>APPL</string>
	<key>CFBundleShortVersionString</key><string>$version</string>
	<key>CFBundleVersion</key><string>$version</string>
	<key>LSMinimumSystemVersion</key><string>$minimum</string>
	<key>NSHighResolutionCapable</key><true/>
	<key>NSPrincipalClass</key><string>NSApplication</string>
</dict>
</plist>
PLIST
# Nested code first, then the bundle (ad-hoc: no Developer ID).
# The resource package's dylib keeps its own signature: the game checks its digest at launch.
find "$inst/Contents/Resources/kit" -path '*/.rrv-resource-packages' -prune -o \
    -type f \( -perm -u+x -o -name '*.dylib' \) -print0 |
    xargs -0 -n1 codesign --force --sign - --timestamp=none 2>/dev/null || true
codesign --force --sign - --timestamp=none "$inst"

print "== no game material in the installer"
python3 "$repo/scripts/export_public.py" --check-tree "$inst/Contents/Resources" 2>&1 | grep -v "symlink" | tail -3 || true
if find "$inst" -path '*generated*' -o -name 'SLUS_*' -o -name '*.elf' | grep -q .; then
    print -u2 "!! game material in the installer"; exit 1; fi
zip="$out/Magatama-$version-macOS-Installer.zip"
rm -f "$zip"; (cd "$out" && ditto -c -k --keepParent "Magatama Installer.app" "$zip")
print "== done: $zip ($(du -h "$zip" | cut -f1), macOS $minimum or later)"
