## October 29 update: stretched font correction verified

The save/load glyphs now retain their original proportions in widescreen. The classifier recognizes the original PSMT4 UV/RGBAQ/XYZF2 font format with the default and observed menu depth/alpha states. It preserves full-screen fades, tiled lighting and framebuffer-feedback coverage.

The rebuilt21:9 load-menu/completion captures are corrected; two matched battle captures remain byte-identical. The final4:3 run has all four captures byte-identical to its control. 1,864 packet checks and5,214 actual-trace checks pass under sanitizers. The normal binary is ready, with user saves and preferences unchanged. Historical pending-build notes below are superseded for these specific scenes; this does not claim every menu/effect throughout the game.

Proof: ~/Documents/Codex/2026-10-22/i-x20/outputs/magatama-text-aspect-20261029/final-report.md.

Normal launch honors saved aspect/preferences, with no rebuild required:
```sh
cd ~/magatama/runtime && MAGATAMA_SOUND_NATIVE=1 scripts/magatama_run.sh
```

## October 28 update: battle target icon

The target cursor now keeps its world-projected anchor while retaining the original HUD proportions and two-layer pulse animation. The original camera had already placed it in widescreen coordinates; the HUD transform was pulling that anchor inward again. A wrapper around original USA25D948 compensates its center before the existing HUD sizing. Off/4:3 are byte-identical. 840original-code cases pass with worst final center error0.3541native UI pixels, plus camera and battle regressions. Rebuild before testing the exact battle; no new in-game after capture is claimed. Proof: Codex `outputs/magatama-target-20261028/target-alignment-report.md`.

## October 28 update: menu restart audit

All 18 visible Terminal controls have been audited and exercised by menu-handler tests. Five apply live: Fullscreen, Integer scaling, FXAA, Sharpening and Metal HUD. The other 13 currently need restart because their setup has no current live path. All explicit saved renderer/input/pacing/fullscreen options now reach Terminal startup; previously only aspect and scale did. Explicit environment overrides still win. App-only Write logs is hidden for Terminal launches, which keep diagnostic logs. Full option-by-option evidence and limitations are in Codex `outputs/magatama-audio-20261028/menu-restart-audit.md`. These are headless/source checks, not in-game visual tests of each toggle.

## October 28 update: Restart next to Quit

After rebuilding, Escape now shows **Resume · Quit · Restart**. Restart asks about unsaved progress, shuts down the current game, then relaunches with saved settings. This works through `scripts/magatama_run.sh` as well as the app bundle. It does not rebuild source. No in-game restart was performed during manual playtesting; menu, capability and supervisor harness checks pass. Proof: Codex `outputs/magatama-20261028/restart-proof-index.json`.

## October 28 update: Escape menu and lighting tiles

The Escape menu now has **Aspect ratio** (Display) and **Rendering resolution** (Rendering). They save to `~/magatama/magatama.ini` on Terminal launches and apply after quitting and relaunching. The picture-fit-only control is hidden. Use the normal native-audio command without an explicit aspect/scale assignment to honor menu choices:

```sh
cd ~/magatama/runtime && scripts/magatama_build.sh && MAGATAMA_SOUND_NATIVE=1 scripts/magatama_run.sh
```

A packet-filter correction keeps tiled untextured world lighting full-width rather than squeezing it with the HUD. Captured native 64×32 lighting tiles reproduce the old bug and pass after the change, including fragmented submissions. Menu/settings suites, sanitized packet fixtures/nine traces, 334 actual camera checks and 10,137 focused battle checks pass. Normal product build and same-battle visual validation are pending. Proof: `outputs/magatama-20261028/proof-index.json` in the Codex workspace.

# Magatama variable aspect ratio

Status: opt-in source implementation with offline validation. The normal game binary was not rebuilt or replaced during the user's manual playtest. First in-game visual validation and the normal product link remain pending. No game was launched, no replay queued, and no card was accessed by this work.

## Use on the next launch

Close the current game first, then run:

```sh
cd ~/magatama/runtime && scripts/magatama_build.sh && MAGATAMA_SOUND_NATIVE=1 MAGATAMA_ASPECT=auto scripts/magatama_run.sh
```

`auto` starts with a 16:9 window and follows the actual drawable ratio when resizing or entering fullscreen. An explicit `RRV_WINDOW_SIZE` still controls the initial size. Fixed modes are `4:3`, `16:9`, `16:10`, and `21:9`; replace `auto` in the command. Fixed modes retain their ratio inside a differently shaped window. With `MAGATAMA_ASPECT` unset or `off`, the new path is inactive and existing defaults apply. `MAGATAMA_ASPECT=4:3` explicitly requests original proportions. FRAME448 remains separate and off; prompts are unchanged.

## Implementation

The main camera calls Nocturne's original SDK screen-parameter builder with a temporary relative aspect adjustment. It retains the current FOV and vertical field of view, near/far planes and depth behavior. The original parameter is restored after each call, preventing accumulated scaling. Secondary and offscreen camera records retain native projection.

A Nocturne-specific GIF filter fits eligible direct UI vertices into a centered original 4:3 plane. Wider windows reveal more world horizontally while the HUD retains its original physical shape and remains centered as a group. Portrait windows fit the UI vertically. The filter preserves texture coordinates, depth, fog, ADC bits, packet order, tags, image transfers and control writes. PATH1 world vertices, offscreen render targets, recognized framebuffer feedback, clears and full-screen covers are excluded from UI scaling. State lives in a backend-owned sidecar, leaving Backend and PS2Runtime layouts unchanged. State classification is conservative and still needs visual coverage, especially fragmented untextured UI, movies and secondary cameras.

## Offline evidence

- 334 assertions against the actual USA SDK camera builder, covering 45 combinations of camera record, FOV and aspect mode, plus auto sizing, invalid configuration and HUD proportions.
- All 10,137 existing focused battle assertions pass with fresh patches: effect/history/rotation, colors, AI/particles, ice/light and trail colors.
- Packet fixtures and nine existing recorded GIF traces pass AddressSanitizer and UndefinedBehaviorSanitizer: 356,729 assertions, mostly byte-preservation checks. This verifies parsing and preservation; it does not verify final rendered appearance.
- The four modified integration translation units compile using the existing pinned build flags. This is not a full product link or in-game validation.
- An initial offline fixture exposed a temporary Backend layout change. It was removed in favor of sidecar state; the Backend header is byte-for-byte unchanged. Corrected runtime tests pass. The initial failure log is retained.

Reproduce runtime checks without rebuilding the normal binary:

```sh
cd ~/magatama/runtime
python3 scripts/magatama_test_aspect.py --regressions
```

Packet fixtures can be compiled into a temporary directory with `clang++ -std=c++17 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -I src/gs-backend tests/nocturne_aspect_packets_test.cpp -o /tmp/magatama-aspect-packets`, then run that executable with selected read-only `.gs` trace paths as arguments. The trace list and per-trace counts are in `aspect-packet-final-tests.log`.

Proof index: `~/Documents/Codex/2026-10-22/i-x20/outputs/magatama-aspect-20261027/aspect-proof-index.json`. Test logs remain under the neighboring `magatama-20261027` output directory. No before/after screenshots are claimed.

## First in-game checks

Check logos, FMV framing, title and save/load menus; field and battle HUD shape/placement; dialogue and menu backgrounds; doors and geometry at the expanded screen edges; resizing and fullscreen; and explicit 4:3 regression. Check 16:9 and ultrawide separately. Correct projection alone does not establish correct culling or every UI/effect packet. Any remaining stretched elements need a captured scene and targeted follow-up.

## Original-code references

Locked USA ELF SHA256: `a26e86e59cab447d9b1839ec11ab82b1557230bcc99746b78e3b5d4359dcc6ff`. Main screen record `0x2F59B0`, aspect `0x2F59B4`, builder `0x2BA070`; the generated product registers that builder.

The matching decomp revision is `05997a758ae7008a3fc064862db9dcff5772309e`: [kernel drawing code](https://github.com/Megami-Decomps/smt3-decomp/blob/05997a758ae7008a3fc064862db9dcff5772309e/src/smt3/kernel/dds3KernelDraw.c) and [SDF camera code](https://github.com/Megami-Decomps/smt3-decomp/blob/05997a758ae7008a3fc064862db9dcff5772309e/src/smt3/sdf/sdf3D.c). The [official PCSX2 USA patch](https://github.com/PCSX2/pcsx2_patches/blob/main/patches/SLUS-20911_E8FCF8EC.pnach) independently corroborates the main aspect address. Optional unrelated patch entries were not applied.
