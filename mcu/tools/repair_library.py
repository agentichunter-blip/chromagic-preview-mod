#!/usr/bin/env python3
"""Create a corrected staging copy; never alter the source library or a device."""
import argparse
from collections import Counter, defaultdict
import csv
import hashlib
import json
from pathlib import Path
import re
import shutil
import unicodedata

from PIL import Image, ImageDraw, ImageFont
from prepare_box_art import convert
from stage_library import audit, digest

LOGO = bytes.fromhex('ceed6666cc0d000b03730083000c000d0008111f8889000edccc6ee6dddd d999bbbb67636e0eecccdddc999fbbb9333e')


def global_sum(data):
    return (sum(data) - sum(data[0x14e:0x150])) & 65535


def repair_rom(data, status):
    """Only header-checksum repair or independently checksum-verified overdumps.

    Do not infer mappers, ROM size codes, missing bytes or encrypted formats.
    """
    if len(data) < 0x150 or data[0x104:0x134] != LOGO:
        return data, ''
    if status == 'invalid_header_checksum':
        fixed = bytearray(data)
        fixed[0x14d] = (-sum(data[0x134:0x14d]) - 25) & 255
        return bytes(fixed), 'correct_header_checksum_byte'
    code = data[0x148]
    if status != 'file_size_mismatch' or code > 8:
        return data, ''
    size = 32768 << code
    if size >= len(data):
        return data, ''
    prefix, tail = data[:size], data[size:]
    if global_sum(prefix) != int.from_bytes(data[0x14e:0x150], 'big'):
        return data, ''
    if len(set(tail)) == 1 and tail[0] in (0, 255):
        return prefix, 'trim_checksum_verified_padding'
    if len(data) % size == 0 and all(data[i:i+size] == prefix for i in range(size, len(data), size)):
        return prefix, 'trim_checksum_verified_repeated_image'
    return data, ''


def title_key(name):
    name = re.split(r' \(| \[', name)[0].casefold()
    name = re.sub(r', (the|a|an)$', '', name)
    name = re.sub(r'^(the|a|an) ', '', name)
    return re.sub('[^a-z0-9]', '', unicodedata.normalize('NFKD', name))


def title_card(name, system, destination):
    # Draw at device resolution. No fabricated publisher logos or cover claims.
    image = Image.new('RGB', (72, 80), '#182018')
    draw = ImageDraw.Draw(image)
    font = ImageFont.load_default(size=9)
    small = ImageFont.load_default(size=8)
    accent = '#a7cf70' if system == 'GB' else '#91c9e5'
    draw.rectangle((0, 0, 71, 11), fill=accent)
    draw.text((4, 1), system, font=small, fill='#182018')
    title = re.split(r' \(| \[', Path(name).stem)[0].replace('_', ' ')
    lines, line = [], ''
    for word in title.split():
        candidate = (line + ' ' + word).strip()
        if draw.textlength(candidate, font=font) <= 64:
            line = candidate
            continue
        if line: lines.append(line)
        line = ''
        for char in word:
            if draw.textlength(line + char, font=font) > 64:
                lines.append(line); line = ''
            line += char
    if line: lines.append(line)
    if len(lines) > 5:
        lines = lines[:5]
        while draw.textlength(lines[-1] + '...', font=font) > 64:
            lines[-1] = lines[-1][:-1]
        lines[-1] += '...'
    for index, line in enumerate(lines):
        draw.text((4, 15 + index * 10), line, font=font, fill='#f2f3df')
    draw.line((4, 68, 67, 68), fill=accent)
    draw.text((4, 70), 'TITLE CARD', font=small, fill=accent)
    destination.parent.mkdir(parents=True, exist_ok=True)
    image.save(destination)


def reason(row, data):
    status = row['header_preflight']
    if row['rom'].startswith('[BIOS]'):
        return 'System/boot ROM; not a standalone cartridge game'
    if len(data) < 0x150:
        return 'Too small to contain a cartridge header'
    if status == 'file_size_mismatch':
        return 'Size disagrees with header; cannot infer missing data or intended bank layout safely'
    if status == 'invalid_header_or_format':
        return 'Nonstandard or encrypted cartridge header; format-specific support needed'
    if status == 'unsupported_mapper_or_geometry':
        mapper = data[0x147]
        special = {0x20:'MBC6', 0x22:'MBC7 sensor/EEPROM', 0xfc:'Camera', 0xfd:'TAMA5', 0xfe:'HuC3'}
        if mapper in special:
            return special[mapper] + ' is not implemented by this FPGA virtual cartridge'
        if data[0x148] == 8:
            return '8 MiB ROM exceeds the current 4 MiB virtual-cartridge limit'
        return f'Unsupported mapper/ROM/RAM geometry ({data[0x147:0x14a].hex()}); not safe to guess'
    return status


def write_csv(path, rows, fields):
    with path.open('w', newline='', encoding='utf-8') as stream:
        writer = csv.DictWriter(stream, fieldnames=fields)
        writer.writeheader(); writer.writerows(rows)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--input', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--preflight', type=Path, required=True)
    parser.add_argument('--title-cards', action='store_true')
    args = parser.parse_args()
    source, output = args.input.resolve(), args.output.resolve()
    if output == source or output.is_relative_to(source):
        parser.error('Output must be a new folder outside input')
    rows = list(csv.DictReader((source/'reports/library-manifest.csv').open()))
    # Validate manifest paths before any copy or writes.
    for row in rows:
        for field in ('sd_rom', 'sd_artwork'):
            if row[field] and not (source/row[field]).resolve().is_relative_to(source):
                parser.error('Manifest path escapes input')
        if digest(source/row['sd_rom']) != row['rom_sha256']:
            parser.error('Input ROM differs from its manifest: ' + row['rom'])
    shutil.copytree(source, output)
    reports = output/'reports'
    shutil.copytree(reports, output/'previous-reports')
    covers = defaultdict(list)
    for row in rows:
        if row['sd_artwork']: covers[title_key(row['rom'])].append(row)
    for index, row in enumerate(rows, 1):
        path = output/row['sd_rom']
        data = path.read_bytes()
        fixed, action = repair_rom(data, row['header_preflight'])
        row['source_rom_sha256'] = row['rom_sha256']
        row['original_header_preflight'] = row['header_preflight']
        row['repair'] = action
        if action:
            path.write_bytes(fixed)
            if audit(args.preflight.resolve(), 'rom', [path])[str(path)] != 'ok':
                path.write_bytes(data); row['repair'] = ''
            else:
                row['rom_sha256'] = digest(path)
                row['rom_bytes'] = str(len(fixed))
        row['cover_kind'] = 'supplied_box_art' if row['sd_artwork'] else 'missing'
        row['cover_reference_rom'] = ''
        if not row['sd_artwork']:
            matches = covers.get(title_key(row['rom']), [])
            relative = Path('CHROMAGIC/ART')/Path(row['sd_rom']).relative_to('CHROMAGIC/BACKUPS')
            art = output/(str(relative)+'.cma')
            if matches:
                # Exact normalized title only; record that this is another edition.
                ref = sorted(matches, key=lambda r: (r['system'] != row['system'], r['rom']))[0]
                art.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(source/ref['sd_artwork'], art)
                row['cover_kind'] = 'same_title_other_edition'
                row['cover_reference_rom'] = ref['rom']
                for key in ('source_url','source_filename','source_game','source_region','source_database_id','source_art_type','source_art_sha256'):
                    row[key] = ref.get(key, '')
            elif args.title_cards:
                png = reports/'title-cards'/Path(row['sd_rom']).relative_to('CHROMAGIC/BACKUPS').with_suffix('.png')
                title_card(row['rom'], Path(row['sd_rom']).parts[2], png)
                convert(png, art)
                row['cover_kind'] = 'generated_title_card'
            if matches or args.title_cards:
                row['sd_artwork'] = art.relative_to(output).as_posix()
                row['conversion'] = 'converted'
        if index % 500 == 0: print(f'Processed {index}/{len(rows)}', flush=True)
    result = audit(args.preflight.resolve(), 'rom-full', [output/r['sd_rom'] for r in rows])
    art_result = audit(args.preflight.resolve(), 'art', [output/r['sd_artwork'] for r in rows if r['sd_artwork']])
    if any(value != 'ok' for value in art_result.values()):
        raise RuntimeError('Converted art rejected by firmware')
    for row in rows:
        status = result[str(output/row['sd_rom'])]
        row['loader_preflight'] = status
        row['header_preflight'] = 'ok' if status.startswith('ok') else status
        row['remaining_issue'] = '' if status.startswith('ok') else reason(row, (output/row['sd_rom']).read_bytes())
    fields = list(dict.fromkeys(key for row in rows for key in row))
    write_csv(reports/'library-manifest.csv', rows, fields)
    write_csv(reports/'rom-repairs.csv', [r for r in rows if r['repair']], fields)
    write_csv(reports/'rom-header-exceptions.csv', [r for r in rows if r['remaining_issue']], fields)
    write_csv(reports/'global-checksum-warnings.csv', [r for r in rows if r['loader_preflight']=='ok_global_checksum_warning'], fields)
    write_csv(reports/'missing-covers.csv', [r for r in rows if r['cover_kind'] in ('missing','generated_title_card')], fields)
    write_csv(reports/'recovered-covers.csv', [r for r in rows if r['cover_kind']=='same_title_other_edition'], fields)
    summary = dict(roms=len(rows), repaired_roms=sum(bool(r['repair']) for r in rows),
                   loader_checks=dict(Counter(r['loader_preflight'] for r in rows)),
                   cover_types=dict(Counter(r['cover_kind'] for r in rows)),
                   all_art_files_accepted_by_firmware_decoder=True,
                   originals_unchanged=True, hardware_tested=False, flashed=False,
                   required_firmware='1.0.1-art2', recommended_firmware='1.0.1-art4')
    # Recheck each final ROM against its final manifest, including repaired copies.
    for row in rows:
        if digest(output/row['sd_rom']) != row['rom_sha256']:
            raise RuntimeError('Final ROM verification failed')
        if digest(source/row['sd_rom']) != row['source_rom_sha256']:
            raise RuntimeError('Original ROM changed during preparation')
    files = sorted(p for p in (output/'CHROMAGIC').rglob('*') if p.is_file())
    files += sorted(p for p in (output/'extras').rglob('*') if p.is_file())
    (reports/'SHA256SUMS').write_text(''.join(f'{digest(p)}  {p.relative_to(output).as_posix()}\n' for p in files))
    (reports/'summary.json').write_text(json.dumps(summary, indent=2)+'\n')
    (output/'README.txt').write_text('''ChroMagic corrected library — recommended firmware 1.0.1-art4

Minimum firmware is 1.0.1-art2. The art3 review fixes console warning visibility.

This is a local staging folder. No device or SD card was written.
When ready, copy CHROMAGIC to the SD root. Hold Up/Down for accelerated
navigation and the letter indicator; A selects; B returns to the parent.

All supplied versions remain present. Original source ROMs and the previous
staging folder are unchanged. Only copies listed in reports/rom-repairs.csv
were modified. Header checksum repair is not a repair of unknown gameplay bugs.
Padding/repetition was removed only when the retained image matches its stored
global checksum. Mapper types, size fields and game code were not guessed.

reports/library-manifest.csv is the current authoritative inventory.
reports/rom-header-exceptions.csv lists unresolved entries and reasons.
reports/global-checksum-warnings.csv lists nonfatal whole-ROM checksum warnings.
The new firmware permits these checksums, as real Game Boy hardware does;
header checks and transfer-error checks remain enforced. Acceptance is not proof
of dump integrity or gameplay compatibility. Physical testing is still pending.

Cover provenance is retained. Recovered covers may be from another edition of
the same title. Generated TITLE CARD images are text previews, not box art.
reports/missing-covers.csv still lists games without actual cover artwork.
previous-reports retains the old staging reports for comparison only.
Supplied factory saves remain under extras; they are not activated.
''')
    print(json.dumps(summary, indent=2), flush=True)


if __name__ == '__main__':
    main()
