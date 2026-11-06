# Covers and gameplay previews

Supply your own images and videos. The firmware download/source does not include a media pack. Keep source media and generated assets outside this repository.

Install the pinned Pillow dependency as described in [building.md](building.md). Video conversion also requires FFmpeg on PATH. Run these examples from the repository root.

## Convert individual files

```sh
.venv-media/bin/python mcu/tools/convert_media.py cover \
  /path/to/cover.png /path/to/SD/CHROMAGIC/ART/GB/Example.gb.cma

.venv-media/bin/python mcu/tools/convert_media.py video \
  /path/to/gameplay.mp4 /path/to/SD/CHROMAGIC/VIDEO/GB/Example.gb.cmv
```

Use a clip at least ten seconds long. Conversion skips the first two seconds and produces eight seconds at 12 fps, 128×92 RGB565, without audio. Images and clips retain their proportions with black padding. Each clip is about 2.26 MB; allow enough SD space for your collection.

Paths mirror the game path beneath `CHROMAGIC/BACKUPS`, retaining its extension:

```text
CHROMAGIC/BACKUPS/GB/Example.gb
CHROMAGIC/ART/GB/Example.gb.cma
CHROMAGIC/VIDEO/GB/Example.gb.cmv

CHROMAGIC/BACKUPS/GBC/Favorites/Example.gbc
CHROMAGIC/ART/GBC/Favorites/Example.gbc.cma
CHROMAGIC/VIDEO/GBC/Favorites/Example.gbc.cmv
```

Match spelling and case exactly. Copy only generated ART/VIDEO files into the corresponding SD directories; keep existing games and saves. The firmware also reads legacy 72×80 CMAR v1 covers produced by `prepare_box_art.py`. New covers use CMAR v2.

## Batch tools

`prepare_box_art.py library --help` describes legacy cover matching. `prepare_previews.py --help` describes the advanced staged-library workflow: it expects `CHROMAGIC/BACKUPS`, a `reports/library-manifest.csv` from the staging tools, originals referenced by that manifest, and source videos grouped under `GB` and `GBC`. It is not a generic drag-and-drop converter; individual conversion above needs no manifest.

Batch matching may select another region/revision of the same title and leaves ambiguous matches unresolved. Review its conversion report before copying results. The library staging, filtering, and repair tools are optional utilities; they are not part of installing the firmware.

## File formats

- CMAR v2: 16-byte little-endian header `<4sBBHHHI>`, magic `CMAR`, version 2, format 1, width 128, height 92, reserved 0, frame byte count; then one RGB565 frame.
- CMVP v1: 24-byte little-endian header `<4sBBHHHIII>`, magic `CMVP`, version 1, format 1, width, height, fps, frame count, frame bytes, reserved 0; followed by raw RGB565 frames.

Converters replace the transparent-magenta key with a neighboring opaque color. The player reads frames incrementally rather than loading a full clip into memory. Fast scrolling suppresses media reads; failed playback falls back to the cover.
