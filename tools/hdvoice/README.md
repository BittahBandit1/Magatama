# HD voice acting (experimental)

Plays *Shin Megami Tensei III: Nocturne HD Remaster*'s voice acting on the PS2 game's own text pages.
Magatama ships none of it: the voices come from **your own copy of Nocturne HD**, pulled out once with the
script here and kept on your machine.

## How it works

Nocturne HD kept the PS2 game's event and field scripts. Its only change to the text was to swap each
voiced page for a tag naming that page's voice clip (`<E601_L0000>`). `hdvoice_extract.py` reads the
PS2 script from your disc image and the HD script from your HD install. It matches them message by
message and page by page, then writes a table (`map.tsv`, keyed by a hash of the PS2 page's text)
plus the clips. In the game, Magatama watches the message windows and plays the clip for whatever
page is on screen.

## Setup

1. Install [UnityPy](https://github.com/K0lb3/UnityPy) (`pip install UnityPy`) and ffmpeg.
2. Run:

   ```
   python3 tools/hdvoice/hdvoice_extract.py --disc <folder with DDS3.DDT and DDS3.IMG> \
       --hd "<...>/steamapps/common/smt3hd" [--lang en|ja|both] [--compact]
   ```

   By default it writes to `~/magatama/hdvoice/out`. Clips are stored mono and lossless, at about
   half HD's size. `--compact` stores them as IMA ADPCM instead: about an eighth of the size, with a
   little hiss. `--lang en` (or `ja`) keeps only one language. `--only e601,f024` limits the run to
   some scripts.
3. In Magatama's menu: **Audio → HD voice acting → English / Japanese**, then restart.

Magatama looks for the voices in a `hdvoice` folder next to its settings file (`magatama.ini`), then
in `~/magatama/hdvoice/out`. To point it elsewhere, set `MAGATAMA_HD_VOICE_DIR`.

## Status

- Story events: working. The opening is fully voiced, line by line.
- Field conversations: supported by the extractor (HD bundles `fNNN`, clips in `eNNN`), not yet tested.
- `MAGATAMA_HD_VOICE_LOG=1` prints each page and which clip played.
  `MAGATAMA_HD_VOICE_AUTO=1` presses Cross after each spoken line; it's only for recording demos.
