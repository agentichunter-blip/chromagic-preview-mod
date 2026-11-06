#!/usr/bin/env python3
"""Stage a local ROM/art manifest for ChroMagic. Never writes to a device."""
import argparse
from collections import Counter
import csv
import hashlib
import io
import json
from pathlib import Path
import shutil
import subprocess

from prepare_box_art import convert

SYSTEMS = {'Nintendo - Game Boy': 'GB', 'Nintendo - Game Boy Color': 'GBC'}


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def group(name):
    title = name.replace('_', ' ').lstrip().upper()
    first = title[:1]
    return 'G-L' if 'G' <= first <= 'L' else 'M-R' if 'M' <= first <= 'R' else 'S-Z' if 'S' <= first <= 'Z' else '0-F'


def audit(tool, kind, paths):
    result = subprocess.run([str(tool), kind], input=''.join(str(p) + '\n' for p in paths),
                            capture_output=True, text=True, check=True)
    statuses = {}
    for line in result.stdout.splitlines():
        status, path = line.split('\t', 1)
        statuses[path] = status
    if len(statuses) != len(paths):
        raise ValueError('Preflight tool returned an incomplete inventory.')
    return statuses


def safe_child(root, name):
    target = (root / name).resolve()
    if not target.is_relative_to(root.resolve()) or '\n' in str(target) or '\r' in str(target) or '\t' in str(target):
        raise ValueError(f'Invalid source path: {name}')
    return target


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--preflight', type=Path, required=True)
    args = parser.parse_args()
    source, output = args.source.resolve(), args.output.resolve()
    manifest = (source / 'boxart-manifest.csv').read_bytes()
    rows = list(csv.DictReader(io.StringIO(manifest.decode('utf-8-sig'))))
    planned, names = [], set()
    for row in rows:
        system = SYSTEMS[row['system']]
        rom = safe_child(source / row['system'], row['rom'])
        if not rom.is_file() or rom.suffix.lower() not in ('.gb', '.gbc'):
            raise ValueError(f'ROM missing or unexpected type: {rom}')
        relative = Path(system) / group(rom.stem) / rom.name
        key = relative.as_posix().casefold()
        if key in names:
            raise ValueError(f'Case-insensitive output collision: {relative}')
        names.add(key)
        device_path = '/sdcard/CHROMAGIC/BACKUPS/' + relative.as_posix()
        if len(device_path.encode('utf-8')) + 5 >= 320 or any(len(p.encode('utf-8')) > 255 for p in relative.parts):
            raise ValueError(f'Path exceeds firmware limits: {relative}')
        image = safe_child(source, row['artwork']) if row['artwork'] else None
        if image and not image.is_file():
            raise ValueError(f'Cover missing: {image}')
        planned.append((row, rom, relative, image))
    actual_roms = {p.resolve() for system in SYSTEMS for p in (source/system).rglob('*') if p.suffix.lower() in ('.gb', '.gbc')}
    if actual_roms != {p[1] for p in planned}:
        raise ValueError('Manifest does not account for every ROM in the source folders.')
    headers = audit(args.preflight.resolve(), 'rom', [p[1] for p in planned])
    output.mkdir(parents=True, exist_ok=False)
    reports = output / 'reports'; reports.mkdir()
    (reports / 'input-boxart-manifest.csv').write_bytes(manifest)
    records, art_paths, checksums = [], [], []
    for index, (row, rom, relative, image) in enumerate(planned, 1):
        target = output / 'CHROMAGIC/BACKUPS' / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(rom, target)
        source_hash, target_hash = digest(rom), digest(target)
        if source_hash != target_hash:
            raise ValueError(f'ROM copy verification failed: {rom}')
        checksums.append((target.relative_to(output).as_posix(), target_hash))
        record = dict(row)
        record.update(sd_rom=target.relative_to(output).as_posix(), rom_sha256=source_hash,
                      rom_bytes=target.stat().st_size, header_preflight=headers[str(rom)],
                      conversion='missing' if image is None else 'converted', sd_artwork='')
        if image:
            art = output / 'CHROMAGIC/ART' / (relative.as_posix() + '.cma')
            record['source_art_sha256'] = digest(image)
            convert(image, art)
            if record['source_art_sha256'] != digest(image):
                raise ValueError(f'Source artwork changed during conversion: {image}')
            record['sd_artwork'] = art.relative_to(output).as_posix()
            art_paths.append(art)
            checksums.append((record['sd_artwork'], digest(art)))
        records.append(record)
        if index % 250 == 0 or index == len(planned):
            print(f'Staged and verified {index}/{len(planned)} ROMs; {len(art_paths)} covers', flush=True)
    art_results = audit(args.preflight.resolve(), 'art', art_paths)
    failures = {p: s for p, s in art_results.items() if s != 'ok'}
    if failures:
        raise ValueError(f'Firmware rejected converted art: {failures}')
    extras = []
    for system in SYSTEMS:
        for path in (source/system).rglob('*'):
            if path.suffix.lower() not in ('.sav', '.rtc'):
                continue
            # Preserve supplied saves separately; never guess which ROM they belong to.
            destination = output/'extras'/SYSTEMS[system]/path.relative_to(source/system)
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(path, destination)
            sha = digest(destination)
            if sha != digest(path): raise ValueError(f'Extra-file verification failed: {path}')
            extras.append(destination.relative_to(output).as_posix())
            checksums.append((extras[-1], sha))
    def write_csv(name, selected):
        fields = list(dict.fromkeys(k for r in records for k in r))
        with (reports/name).open('w', newline='') as stream:
            writer=csv.DictWriter(stream, fieldnames=fields);writer.writeheader();writer.writerows(selected)
    write_csv('library-manifest.csv', records)
    write_csv('missing-covers.csv', [r for r in records if r['conversion']=='missing'])
    write_csv('rom-header-exceptions.csv', [r for r in records if r['header_preflight']!='ok'])
    folder_counts=Counter(str(p[2].parent) for p in planned)
    summary = {
        'roms':len(records), 'covers':len(art_paths), 'missing_covers':len(records)-len(art_paths),
        'rom_bytes':sum(r['rom_bytes'] for r in records), 'converted_art_bytes':sum(p.stat().st_size for p in art_paths),
        'folder_counts':dict(sorted(folder_counts.items())),
        'header_preflight':dict(Counter(r['header_preflight'] for r in records)),
        'all_rom_copies_sha256_verified':True, 'all_art_files_accepted_by_firmware_decoder':True,
        'preserved_extras':extras,
        'source_manifest_sha256':hashlib.sha256(manifest).hexdigest(),
        'source_manifest_unchanged':manifest==(source/'boxart-manifest.csv').read_bytes(),
        'hardware_tested':False, 'flashed':False,
    }
    (reports/'summary.json').write_text(json.dumps(summary,indent=2)+'\n')
    (reports/'SHA256SUMS').write_text(''.join(f'{sha}  {path}\n' for path,sha in sorted(checksums)))
    (output/'README.txt').write_text(f'''ChroMagic box-art SD content — prepared from the supplied collection

Copy the CHROMAGIC folder to the root of your SD card when ready.
This is a staging folder on the computer; no SD card or device was changed.
Use with the previously built ChroMagic 1.0.1-art1 firmware. Nothing was flashed.

{len(records):,} ROMs and {len(art_paths):,} converted covers.
{len(records)-len(art_paths):,} missing covers use the menu's NO COVER placeholder.
Every supplied ROM version is retained. Originals are untouched.

BACKUPS and ART contain mirrored GB/GBC folders, divided into 0-F, G-L, M-R,
and S-Z ranges. Numbers and symbols are in 0-F. The largest folder contains
{max(folder_counts.values())} ROMs. A opens a folder or launches a game; B goes up.
Hold Up/Down for accelerated scrolling and the alphabet indicator.

Reports:
- reports/library-manifest.csv: exact ROM/art associations and original cover provenance.
- reports/missing-covers.csv: games without supplied artwork.
- reports/rom-header-exceptions.csv: files rejected by the firmware's current header decoder.
- reports/summary.json: totals and verification results.
- reports/SHA256SUMS: hashes relative to this staging folder.

All ROM copies were verified byte-for-byte with SHA-256. Every converted cover
was accepted by the actual firmware's artwork decoder. Header acceptance is
not a gameplay test and does not check the complete ROM/global checksum.
No ROM headers or data were patched. Rejected files remain present so no
supplied version is silently discarded; those entries may not launch.

Supplied save/RTC files are preserved under extras, separately from BACKUPS.
Do not replace an existing game save without deciding to use that extra file.
''')
    print(json.dumps(summary,indent=2), flush=True)


if __name__ == '__main__':
    main()
