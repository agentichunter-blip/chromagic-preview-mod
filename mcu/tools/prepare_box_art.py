#!/usr/bin/env python3
"""Prepare 72x80 CMAR v1 covers for the ChroMagic box-art browser."""
import argparse
import json
from pathlib import Path
import struct
import sys
from PIL import Image, ImageOps

WIDTH, HEIGHT = 72, 80
BACKGROUND = (0x18, 0x20, 0x18)
HEADER = struct.pack('<4sBBHHHI', b'CMAR', 1, 1, WIDTH, HEIGHT, 0, WIDTH * HEIGHT * 2)
EXTENSIONS = {'.png', '.jpg', '.jpeg', '.webp', '.bmp'}


def convert(source: Path, output: Path) -> None:
    with Image.open(source) as image:
        image = ImageOps.exif_transpose(image).convert('RGBA')
        image = ImageOps.contain(image, (WIDTH, HEIGHT), Image.Resampling.LANCZOS)
        canvas = Image.new('RGBA', (WIDTH, HEIGHT), BACKGROUND + (255,))
        canvas.alpha_composite(image, ((WIDTH - image.width) // 2, (HEIGHT - image.height) // 2))
        pixels = bytearray()
        rgb = canvas.convert('RGB').tobytes()
        for r, g, b in zip(rgb[0::3], rgb[1::3], rgb[2::3]):
            color = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
            if color == 0xF81F:
                color = 0xF81E
            pixels.extend(struct.pack('<H', color))
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = output.with_name(output.name + '.tmp')
    try:
        temporary.write_bytes(HEADER + pixels)
        temporary.replace(output)
    finally:
        temporary.unlink(missing_ok=True)


def library(rom_root: Path, images: Path, art_root: Path, mapping: dict) -> list[dict]:
    if not rom_root.is_dir() or not images.is_dir():
        raise ValueError('ROM root and image folder must both exist.')
    candidates = [p for p in images.rglob('*') if p.is_file() and p.suffix.lower() in EXTENSIONS]
    by_relative, by_name = {}, {}
    for image in candidates:
        key = image.relative_to(images).with_suffix('').as_posix().casefold()
        by_relative.setdefault(key, []).append(image)
        by_name.setdefault(image.stem.casefold(), []).append(image)
    result = []
    for rom in sorted(rom_root.rglob('*')):
        if not rom.is_file() or rom.suffix.lower() not in {'.gb', '.gbc'}:
            continue
        relative = rom.relative_to(rom_root)
        key = relative.as_posix()
        entry = {'rom': key}
        if key in mapping:
            match = Path(mapping[key])
            matches = [match if match.is_absolute() else images / match]
        else:
            matches = []
            # Prefer full relative identity, retaining the ROM extension when present.
            for lookup, names in ((by_relative, [key, relative.with_suffix('').as_posix()]),
                                  (by_name, [rom.name, rom.stem])):
                for name in names:
                    matches = lookup.get(name.casefold(), [])
                    if matches:
                        break
                if matches:
                    break
        if not matches:
            entry['status'] = 'missing'
        elif len(matches) != 1:
            entry.update(status='ambiguous', candidates=[str(p) for p in matches])
        else:
            destination = art_root / (key + '.cma')
            try:
                convert(matches[0], destination)
                entry.update(status='converted', image=str(matches[0]), output=str(destination))
            except (OSError, ValueError) as exc:
                entry.update(status='error', error=str(exc))
        result.append(entry)
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='command', required=True)
    single = commands.add_parser('convert', help='Convert one cover')
    single.add_argument('image', type=Path)
    single.add_argument('output', type=Path)
    batch = commands.add_parser('library', help='Match covers to a ROM folder tree')
    batch.add_argument('--rom-root', type=Path, required=True)
    batch.add_argument('--images', type=Path, required=True)
    batch.add_argument('--art-root', type=Path, required=True)
    batch.add_argument('--map', type=Path, help='JSON: ROM relative path -> cover path (relative to --images or absolute)')
    batch.add_argument('--report', type=Path, required=True)
    args = parser.parse_args()
    try:
        if args.command == 'convert':
            convert(args.image, args.output)
            print(f'Created {args.output} (72x80 RGB565)')
            return 0
        mapping = json.loads(args.map.read_text()) if args.map else {}
        if not isinstance(mapping, dict) or not all(isinstance(k, str) and isinstance(v, str) for k, v in mapping.items()):
            raise ValueError('Mapping must be a JSON object of ROM paths to image paths.')
        result = library(args.rom_root, args.images, args.art_root, mapping)
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(json.dumps(result, indent=2) + '\n')
        print(', '.join(f'{sum(e["status"] == state for e in result)} {state}'
                        for state in ('converted', 'missing', 'ambiguous', 'error')))
        return int(any(e['status'] in {'ambiguous', 'error'} for e in result))
    except (OSError, ValueError) as exc:
        print(f'Error: {exc}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
