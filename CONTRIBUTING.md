# Contributing

Thanks for helping with Magatama. A few ground rules keep this project legal and maintainable.

## Hard rules

- **No game material.** Never commit, attach or link to game files: disc images, ELF/IRX binaries, extracted assets,
  captures (`.gsr`, `.gs`, VRAM and RAM dumps), BIOS images, or anything generated from the executable (`generated/`,
  `generated-nocturne/`, the battle/effect routines from `scripts/magatama_generate_bezier.py`). Issues and PRs that
  contain any are closed and removed. Run logs (`logs/run-*.log`) are fine (they contain none).
- **No copy-protection work** and no help obtaining the game illegally.
- **GPL-3.0.** Your contribution is licensed under the project's licence. If you reuse PCSX2 code, keep the upstream
  licence header and say which revision, file and function it comes from.
- **Game behaviour changes only in the override layer** (`src/product/patches.cpp`) or in the runtime
  (`third_party/ps2recomp/ps2xRuntime`), never in generated code. Anything that changes what the game does is an
  explicit, documented, user-selectable enhancement (widescreen, draw distance and the like).
- **Settings:** every user toggle lives in the settings file (`magatama.ini`) and in the in-game menu (one schema,
  `src/app/fukami_settings.cpp`).
- **iOS and iPadOS run landscape only.** The game, and the launcher in front of it, are always horizontal on iPhone
  and iPad: landscape left and landscape right, never portrait or upside down. The orientation lists in
  `cmake/ios/Info.plist.in`, SDL's orientation hint in `src/app/magatama_ios.mm` and the launcher's
  `supportedInterfaceOrientations` in `src/launcher/ios/MagatamaLauncher.swift` stay landscape-only, and
  `scripts/magatama_build_ios.sh` refuses to package an app whose Info.plist allows any portrait orientation.

## Working on it

1. Read [BUILDING.md](BUILDING.md) and [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).
2. Run the asset-free suite ([docs/TESTING.md](docs/TESTING.md)) before and after your change.
3. For anything that touches timing, rendering or the runtime, say how you verified it on real game data and what you
   compared against. PCSX2 is the reference for intended PS2 behaviour; find the **first divergence** rather than
   guessing from the final picture.
4. One pull request per change, small logical commits, docs and tests with the code.

## Reporting a bug

Use the bug-report template: revision or build number, platform and device, what you did, the newest run log
(`logs/run-*.log`; on iPhone/iPad in the app's folder in Files, on Android in `Documents/Magatama/logs`). Do not attach
game data.

AI-assisted contributions are welcome if a person has read, run and understood them; say so in the PR.
