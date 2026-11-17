# Building Magatama

**The repository contains no game code.** Everything game-derived is generated from your own `SLUS_209.11` (the
executable in the root of your Shin Megami Tensei: Nocturne (USA) disc) on your computer, into folders git ignores.
Never commit, upload or share them.

Magatama is a developer preview, and its build still shows its origins: the macOS build uses the PCSX2 GS bridge that a
[Fukami](https://github.com/danisandoval/fukami) checkout builds, and the folder layout below is the one the scripts
default to. Every path can be overridden; the scripts' headers list the variables.

```
~/magatama/
  runtime/              this repository
  generated-nocturne/   the translated game code (step 2)
  disc/                 the disc's files (the launchers import them; SLUS_209.11 at the top)
  memcard/ logs/        saves and logs of the macOS build
~/fukami/               a Fukami checkout with its bridge built (macOS only; FUKAMI_DIR overrides)
```

## 0. Tools

- **macOS:** Xcode (with the Metal toolchain and, for iPhone/iPad, the iOS SDK), CMake ≥ 3.21, Ninja, Python 3.9+, Git,
  and Homebrew's `zstd` plus the libraries listed in [docs/FUKAMI_BUILDING.md](docs/FUKAMI_BUILDING.md#0-tools).
- **Android:** the Android NDK r28 or newer and an SDK with build-tools and a platform (Linux x86-64 or macOS host);
  Java for `d8` and `apksigner`.
- **Linux:** see `scripts/magatama_build_linux.sh --packages` (apt, dnf or pacman).

## 1. Check the checkout without any game data

```sh
cmake --preset m0-macos-arm64            # or m0-linux-x86_64
cmake --build build/m0-macos-arm64 --target rrv-asset-free-tests --parallel
python3 scripts/qualify_asset_free.py --build-dir build/m0-macos-arm64
```

## 2. Generate the game code from your ELF

```sh
python3 scripts/magatama_generate.py --elf /path/to/SLUS_209.11
```

This checks the ELF's SHA-256 (only the USA release works), builds the recompiler from `third_party/ps2recomp`, runs it
with `config/magatama/nocturne-recomp.toml.in` and the function list `config/magatama/nocturne-functions.csv`, writes
the translated code to `../generated-nocturne`, and lays out `generated/nocturne/` for the build (the ahead-of-time VU
code, the battle/effect routines the function set leaves out, and the ELF identity the game checks at launch). Re-run
`scripts/magatama_prepare_generated.py` after any regeneration.

## 3. Build

### macOS

Build Fukami's PCSX2 GS bridge in `~/fukami` first ([docs/FUKAMI_BUILDING.md](docs/FUKAMI_BUILDING.md), steps 2 and 4),
then:

```sh
scripts/magatama_build.sh                     # the game: build/magatama/candidate-bin/Fukami
scripts/magatama_build_launcher_mac.sh        # ~/magatama/Magatama Launcher.app (import a disc image, then Play)
scripts/magatama_run.sh [disc-folder]         # or run the game straight from the build tree
```

### iPhone and iPad

```sh
scripts/magatama_build_ios.sh                          # iOS 16+, A12 or newer
MAGATAMA_IOS_LEGACY=1 scripts/magatama_build_ios.sh    # iOS 15, A10/A11
```

It builds its own static dependencies, the PCSX2 Metal bridge for iOS and the game, and writes an unsigned
`Magatama-unsigned.ipa` (and a numbered copy in `~/magatama/ios-builds`, with a `.tipa` for TrollStore). The build refuses
to package an app that allows portrait orientations (Magatama is landscape only on iOS). `MAGATAMA_IOS_SIM=1` builds for the
Simulator instead.

### Android

```sh
ANDROID_NDK=/path/to/ndk ANDROID_SDK=/path/to/sdk scripts/magatama_build_android.sh
```

Stages `deps`, `pcsx2`, `bridge`, `product` and `apk` each resume; name one to start there. The APK is debug-signed with
`~/magatama/android-debug.keystore` (created on first use; `MAGATAMA_ANDROID_KEYSTORE` overrides); keep that file, or a
new build will not install over an old one. On the device, allow "All files access" when asked: the disc, saves,
settings and logs live in `Documents/Magatama`.

### Linux

`scripts/magatama_linux_kit.sh` packs this checkout and the generated code on a Mac; `scripts/magatama_build_linux.sh`
builds it natively on the Linux machine (x86-64 or ARM64) and `scripts/magatama_run_linux.sh` runs it.

## 4. Check a change

Run the asset-free suite (step 1). For anything that touches timing, rendering or the runtime, say in the pull request
what you ran on your own game data and what you compared it against ([docs/TESTING.md](docs/TESTING.md)).

## Layout

| Path | What |
|---|---|
| `src/product` | the override layer: every change to how the game behaves (Nocturne and Ridge Racer V) |
| `src/app` | settings and menu, disc import (`magatama_disc_import.*`), the iOS and Android app shells |
| `src/launcher` | the SwiftUI launchers for macOS and iOS (Android's is `src/app/android/dev/magatama/LauncherActivity.java`) |
| `src/host` | window, input, touch controller, presentation |
| `src/audio`, `third_party/dq8-iop` | sound: the IOP core that runs the game's own sound drivers |
| `third_party/ps2recomp` | recompiler, analyzer and runtime (PS2Recomp-derived; `RRV_CHANGES.md`) |
| `tools/pcsx2-gs-bridge`, `tools/patches` | the PCSX2 bridge and its patches (`ios/`, `android/`, `mobile/` for the ports) |
| `config/magatama` | recompiler configuration and function list for Nocturne, the product workload |
| `generated/` | **local only**: generated from your ELF |

The Ridge Racer V side of the shared runtime is documented in [docs/FUKAMI.md](docs/FUKAMI.md) and
[docs/FUKAMI_BUILDING.md](docs/FUKAMI_BUILDING.md).
