# Magatama

**Magatama** is a native port of **Shin Megami Tensei: Nocturne** (PlayStation 2, USA, `SLUS-20911`) made by static
recompilation. The game's MIPS code is translated ahead of time into C++ on your own computer, from your own disc, and
compiled into a normal application for macOS, iPhone and iPad, Android and Linux, with a PS2 hardware layer (the GS from
[PCSX2](https://pcsx2.net/)) underneath. It is **not an emulator** and it runs no other game.

Magatama is built on [Fukami](https://github.com/danisandoval/fukami), the Ridge Racer V port, and shares its runtime.

> **This repository contains no game code and no game data.** You need your own copy of Shin Megami Tensei: Nocturne
> (USA), dumped from a disc you own. Magatama is not affiliated with, authorised or endorsed by Atlus, Sega or Sony.
> See [LEGAL.md](LEGAL.md).

> **Status: work in progress.** Much of the game plays, but Magatama is a developer preview: there are no official
> releases yet, you build it yourself, and you will find bugs.

## Features

- Native code: the game logic runs as compiled ARM64 or x86-64 code, with no JIT, so it also runs where run-time code
  generation is forbidden (iOS).
- Internal resolution up to 8x, FXAA, CAS sharpening, anisotropic filtering.
- Widescreen: 16:9, 16:10, 21:9 and 32:9 views, with the HUD, menus and text kept at their original proportions.
- 60 FPS mode (experimental): the camera, characters and demons are interpolated between the game's 30 FPS frames.
- The game's own sound drivers, run on an IOP core, for music, effects and movie audio.
- Controllers and keyboard everywhere; an on-screen controller on iPhone, iPad and Android that hides itself while a
  controller is connected.
- An in-game settings menu (triggered with Esc or a three-finger tap).
- Launchers for macOS, iOS and Android that import your disc image (`.iso`, `.chd` or `.zso`) and check it is the
  right one (SHA-256 of `SLUS_209.11`) before anything is copied.
- Right Analog Stick controls the camera, enabled with an R3 click.

## Performance & Compatibility

Performance and compatibility has been tested on many pieces of hardware. For iOS, the minimum for sustained 30 FPS is the A15 Bionic. 
On macOS, the minimum is A18 Pro or M1. Android minimum is Snapdragon 865. Adreno GPU is a requirement. 
x86-64 Builds have not been made or tested at this time.

## Platforms

| Platform | Status |
|---|---|
| **macOS** (Apple silicon) | Main development platform. Built with `scripts/magatama_build.sh`; started with the Magatama Launcher app or `scripts/magatama_run.sh`. |
| **iPhone and iPad** | Unsigned `.ipa` / `.tipa` for sideloading or TrollStore. iOS 16 or later on A12 or newer; a legacy build covers iOS 15 on A10/A11 (iPhone 7/8/X, iPod touch 7). Landscape only. |
| **Android** | arm64 APK, Android 9 or later, Vulkan. Tested on Snapdragon (Adreno) handhelds; Mali GPUs are not supported yet. Works on 60 Hz and 120 Hz screens. |
| **Linux** (x86-64, ARM64) | Native build from a kit (`scripts/magatama_build_linux.sh`); less tested. |
| **Windows** | Not yet. |

## Getting started

There are no prebuilt downloads yet. To build Magatama you need your own `SLUS_209.11` (the executable in the root of
the disc); everything game-derived is generated from it on your computer. See [BUILDING.md](BUILDING.md).

Once built, the launcher's **Import Disc** button takes your disc image. On iPhone and iPad the disc, saves, settings
and logs are in the app's folder in the Files app; on Android they are in `Documents/Magatama`.

## Known Issues & General Notes
- In rare cases, the interpolation does not properly apply upon startup. Many animations still update at 30 frames per second in this glitched state. Reboot the game to fix.
- Button prompts are PlayStation only. In the future, Nintendo Switch and Xbox button prompts will be human-drawn and added into the game.
- On macOS specifically, there is a small chance that the transition to your first battle upon bootup will slightly stutter.
- In order to comply with copyright law, the launchers build the necessary code before the first runtime. This process can take a long time.
- While Linux and Windows support is baked into the project via Fukami, zero work has been done to make those versions functional by me.
Bug reports are welcome, but do not expect support for fixing any issues if you build those versions from source.
- This project as a whole is still experimental. Expect bugs and crashes. Save frequently when possible.
- Interpolation is currently limited to 60 FPS. This limit will be rectified in the future.
- The "Legacy" iOS version is moreso meant for powerful devices on older iOSes, not old processors.
The game will boot on A10 Bionic, but performance is unplayably slow.

## Contributing

Bug reports and pull requests are welcome; read [CONTRIBUTING.md](CONTRIBUTING.md) first. Never attach, upload or link
game files (disc images, the ELF, extracted assets, the generated code).

For bug reports, please try to include a screenshot or video displaying the problem. Be sure to include your OS and hardware specs as well.

## Credits and licence

Magatama is free software under the GNU General Public License v3.0 ([LICENSE](LICENSE)). It stands on:

- [Fukami](https://github.com/danisandoval/fukami) by Dani Sandoval: the runtime, the PCSX2 GS bridge, the app, the
  tooling and the documentation this project grew from ([docs/FUKAMI.md](docs/FUKAMI.md)).
- [PS2Recomp](https://github.com/ran-j/PS2Recomp) by ran-j: the recompiler and runtime.
- [PCSX2](https://pcsx2.net/): the GS renderer and SPU2 core.
- The IOP core from [Sinan Karakaya's PS2Recomp fork](https://github.com/Sinan-Karakaya/PS2Recomp) (as pinned by
  DQ8-Recompiled), which runs the game's own sound drivers (`third_party/dq8-iop`).
- [SDL](https://libsdl.org/) and the other libraries in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

*Shin Megami Tensei* and *Nocturne* are trademarks of Atlus / Sega; *PlayStation* is a trademark of Sony Interactive
Entertainment. They are used only to say which game this software works with.
