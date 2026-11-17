# Legal notes

**Magatama is not affiliated with, authorised, sponsored or endorsed by Atlus, Sega, Sony Interactive Entertainment
or any other rights holder.** *Shin Megami Tensei*, *Nocturne*, *Ridge Racer* and *PlayStation* are trademarks of their
respective owners; they are used here only to say which games this software works with.

## What this project is

Magatama is a compatibility layer: a runtime plus tools that let a Shin Megami Tensei: Nocturne (USA) executable that
**you own** run natively. It is based on Fukami, which does the same for Ridge Racer V, and shares its runtime. It is not
an emulator and it does not run other games.

## What this repository contains

- Source code written for this project, and code derived from GPL-licensed projects (Fukami, PCSX2, PS2Recomp, the
  DQ8-Recompiled IOP core), under the GNU General Public License v3.0 ([LICENSE](LICENSE),
  [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)).
- Configuration for the recompiler: function addresses, sizes and names, memory-mapped addresses and jump-table
  addresses of the supported executable, and SHA-256 fingerprints used to recognise it. No game code.
- **No game code and no game data**: no disc data, executable, IOP modules, textures, models, audio, video or other
  assets, and no PS2 BIOS (the kernel is reimplemented). The translated game code is generated on your computer from
  your own executable (`scripts/magatama_generate.py`) into folders git ignores, and the continuous-integration checks
  fail any change that contains it.

## What you must provide

- A legally obtained copy of Shin Megami Tensei: Nocturne (USA, `SLUS-20911`), dumped by you from your own disc.
  Magatama checks the executable's SHA-256 and refuses anything else.
- Do not ask for, offer or link to game files, or to builds that contain generated game code, in this project's issues,
  discussions or pull requests; such posts are removed.
- Magatama contains no copy-protection circumvention and the project does not accept work on it.

## Takedown and contact

If you represent a rights holder and believe something in this repository should not be here, please open a private
security advisory on GitHub or contact the maintainer listed on the repository profile; we will respond promptly.
