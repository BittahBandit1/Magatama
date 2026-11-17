#!/bin/zsh -f
# Builds "Magatama Launcher.app" (src/launcher/macos, SwiftUI) into ~/magatama: import a disc image, then Play.
#   scripts/magatama_build_launcher_mac.sh
# Needs the Mac product build's disc libraries (build/magatama/librrv-disc-*.a, from scripts/magatama_build.sh) and
# Homebrew's static zstd. The game still runs from the build tree (scripts/magatama_run.sh); the launcher starts it.
repo="${0:A:h:h}"
out="$HOME/magatama/Magatama Launcher.app"
build="$repo/build/magatama"
pcsx2="$repo/build-deps/pcsx2-2.8.2"
zstd="$(ls /opt/homebrew/lib/libzstd.a /usr/local/lib/libzstd.a 2>/dev/null | head -1)"
for f in "$build/librrv-disc-extract.a" "$build/librrv-disc-chdr.a" "$zstd"; do
    [[ -f "$f" ]] || { print -u2 "!! missing $f (run scripts/magatama_build.sh first; brew install zstd)"; exit 1; }
done
tmp="$repo/build/launcher-mac"; rm -rf "$tmp"; mkdir -p "$tmp"
app="$tmp/Magatama Launcher.app"; mkdir -p "$app/Contents/MacOS" "$app/Contents/Resources"

print "== disc import tool"
xcrun clang++ -std=c++20 -O2 -I"$repo/src/app" -I"$pcsx2/3rdparty/libchdr/include" \
    "$repo/src/app/magatama_disc_import.cpp" "$repo/src/app/magatama_disc_import_tool.cpp" \
    "$build/librrv-disc-extract.a" "$build/librrv-disc-chdr.a" "$zstd" -lz \
    -o "$app/Contents/MacOS/magatama-disc-import" || { print -u2 "!! import tool failed"; exit 1; }

print "== launcher (Swift)"
xcrun swiftc -parse-as-library -O -swift-version 5 -target arm64-apple-macos13.0 \
    "$repo/src/launcher/macos/MagatamaLauncherMac.swift" -o "$app/Contents/MacOS/MagatamaLauncher" ||
    { print -u2 "!! launcher failed"; exit 1; }

cat > "$app/Contents/Info.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>CFBundleExecutable</key><string>MagatamaLauncher</string>
	<key>CFBundleIdentifier</key><string>io.github.bittahbandit1.magatama.launcher</string>
	<key>CFBundleName</key><string>Magatama</string>
	<key>CFBundleDisplayName</key><string>Magatama Launcher</string>
	<key>CFBundlePackageType</key><string>APPL</string>
	<key>CFBundleShortVersionString</key><string>1.0</string>
	<key>CFBundleVersion</key><string>1</string>
	<key>LSMinimumSystemVersion</key><string>13.0</string>
	<key>NSHighResolutionCapable</key><true/>
	<key>NSPrincipalClass</key><string>NSApplication</string>
</dict>
</plist>
PLIST
codesign -f -s - "$app/Contents/MacOS/magatama-disc-import" && codesign -f -s - "$app" ||
    { print -u2 "!! codesign failed"; exit 1; }
rm -rf "$out"; cp -R "$app" "$out"
"$out/Contents/MacOS/magatama-disc-import" --check "$HOME/magatama/disc"
print "== done: $out"
