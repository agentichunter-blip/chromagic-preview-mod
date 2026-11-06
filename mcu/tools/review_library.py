#!/usr/bin/env python3
"""Read-only review of original/staged ROMs; write evidence under reports/review."""
import argparse
from collections import Counter
import csv
import hashlib
import json
from pathlib import Path
import re

from stage_library import audit


def sha(data):
    return hashlib.sha256(data).hexdigest()


def declared_size(data):
    if len(data) < 0x150:
        return None
    code = data[0x148]
    if code <= 8:
        return 32768 << code
    return {0x52: 72*16384, 0x53: 80*16384, 0x54: 96*16384}.get(code)


def tail_kind(data, size):
    if size is None or len(data) <= size:
        return ''
    tail = data[size:]
    if len(data) % size == 0 and all(data[i:i+size] == data[:size] for i in range(size, len(data), size)):
        return 'identical_repeated_image'
    if tail == b'\0'*len(tail) or tail == b'\xff'*len(tail):
        return 'uniform_padding'
    return 'contains_other_data'


def classify(row, data):
    name, status = row['rom'], row['header_preflight']
    size = declared_size(data)
    tail = tail_kind(data, size)
    if name.startswith('[BIOS]'):
        category, next_step = 'boot_or_system_rom', 'Exclude from game-failure counts; these are labeled system/boot images'
    elif re.search(r'Action Replay|GameShark|Game Genie|Xploder|SmartCom|Test (Cartridge|Program)|DiTech Interface|Diagnostic System|Doctor GB Card', name, re.I):
        category, next_step = 'accessory_or_diagnostic_candidate', 'Filename indicates accessory/test software; confirm intended device before treating as a game'
    elif status == 'unsupported_mapper_or_geometry' and data[0x147] in (0x20, 0x22, 0xfc, 0xfd, 0xfe):
        category, next_step = 'unsupported_cartridge_hardware', 'Requires new FPGA mapper/peripheral support; do not relabel the mapper'
    elif size and size > 4*1024*1024:
        category, next_step = 'exceeds_virtual_rom_capacity', 'Current ROM window is limited to 4 MiB; do not truncate'
    elif status == 'file_size_mismatch' and tail in ('identical_repeated_image', 'uniform_padding'):
        category, next_step = 'redundant_tail_needs_validation', 'Candidate for a separately tested copy; retained prefix fails stored global checksum, so no automatic repair was applied'
    elif status == 'file_size_mismatch' and size and len(data) < size:
        category, next_step = 'shorter_than_declared', 'Compare with an authoritative dump or format specification; do not invent missing bytes'
    elif status == 'file_size_mismatch' and size and len(data) > size:
        category, next_step = 'extra_nonredundant_data', 'Unique data extends past declared size; investigate mapper/layout, not automatic trimming'
    elif status == 'unsupported_mapper_or_geometry':
        category, next_step = 'header_or_mapper_inconsistency', 'Review the actual mapper and RAM requirements; header changes alone do not prove compatibility'
    else:
        category, next_step = 'nonstandard_header', 'Investigate format-specific decoding; do not substitute a generic logo/header'
    return dict(system=row['system'], rom=name, sd_rom=row['sd_rom'],
                header_preflight=status, category=category,
                actual_bytes=len(data), declared_bytes=size or '',
                mapper_hex=f'{data[0x147]:02x}' if len(data)>=0x150 else '',
                tail=tail, prototype_or_beta=bool(re.search(r'\((Proto|Beta)', name)),
                multicart_name=bool(re.search(r'\b\d+[ -]in[ -]1\b', name, re.I)),
                next_step=next_step)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--originals', type=Path, required=True)
    parser.add_argument('--before', type=Path, required=True)
    parser.add_argument('--library', type=Path, required=True)
    parser.add_argument('--preflight', type=Path, required=True)
    args = parser.parse_args()
    root = args.library.resolve()
    rows = list(csv.DictReader((root/'reports/library-manifest.csv').open()))
    repairs, unresolved = [], []
    for index, row in enumerate(rows, 1):
        original = (args.originals/row['system']/row['rom']).read_bytes()
        before = (args.before/row['sd_rom']).read_bytes()
        after = (root/row['sd_rom']).read_bytes()
        assert original == before, row['rom']
        assert sha(before) == row['source_rom_sha256'], row['rom']
        assert sha(after) == row['rom_sha256'], row['rom']
        if row['repair']:
            assert before != after, row['rom']
            differences = [i for i, (a,b) in enumerate(zip(before, after)) if a != b]
            if row['repair'] == 'correct_header_checksum_byte':
                assert len(before) == len(after) and differences == [0x14d], row['rom']
                checksum = 0
                for value in after[0x134:0x14d]: checksum = (checksum-value-1) & 255
                assert after[0x14d] == checksum, row['rom']
                evidence = 'Only offset 0x014D changed; independently recomputed header checksum agrees'
            else:
                assert not differences and len(after) == declared_size(before), row['rom']
                kind = tail_kind(before, len(after))
                assert kind in ('identical_repeated_image','uniform_padding'), row['rom']
                checksum = sum(after[:0x14e])+sum(after[0x150:])
                assert (checksum & 65535) == int.from_bytes(after[0x14e:0x150], 'big'), row['rom']
                evidence = 'Retained prefix unchanged; '+kind+'; original global checksum agrees'
            repairs.append(dict(rom=row['rom'], repair=row['repair'], original_bytes=len(before),
                                final_bytes=len(after), evidence=evidence,
                                original_sha256=sha(before), final_sha256=sha(after)))
        else:
            assert before == after, row['rom']
        if row['header_preflight'] != 'ok': unresolved.append(classify(row, after))
        if index % 1000 == 0: print(f'Compared {index}/{len(rows)} original and staged files', flush=True)
    # Exercise the current firmware decoder and check stored status, not just counts.
    statuses = audit(args.preflight.resolve(), 'rom-full', [root/r['sd_rom'] for r in rows])
    for row in rows: assert statuses[str(root/row['sd_rom'])] == row['loader_preflight'], row['rom']
    art_statuses = audit(args.preflight.resolve(), 'art', [root/r['sd_artwork'] for r in rows if r['sd_artwork']])
    assert all(s == 'ok' for s in art_statuses.values())
    files_checked = 0
    for line in (root/'reports/SHA256SUMS').read_text().splitlines():
        checksum, relative = line.split('  ', 1)
        assert sha((root/relative).read_bytes()) == checksum, relative
        files_checked += 1
    out = root/'reports/review'; out.mkdir(exist_ok=True)
    for name, records in [('repair-evidence.csv',repairs), ('unresolved-classification.csv',unresolved)]:
        with (out/name).open('w', newline='') as stream:
            writer = csv.DictWriter(stream, fieldnames=list(records[0]) if records else ['rom'])
            writer.writeheader(); writer.writerows(records)
    summary = dict(original_roms_identical_to_initial_stage=len(rows),
                   final_roms_verified=len(rows), repairs_verified=len(repairs),
                   unchanged_roms=len(rows)-len(repairs), current_decoder_results_match_manifest=True,
                   preview_files_verified=len(art_statuses), staged_file_hashes_verified=files_checked,
                   unresolved_categories=dict(Counter(r['category'] for r in unresolved)),
                   automatic_additional_rom_changes=0, hardware_tested=False, flashed=False)
    (out/'summary.json').write_text(json.dumps(summary, indent=2)+'\n')
    print(json.dumps(summary, indent=2), flush=True)


if __name__ == '__main__':
    main()
