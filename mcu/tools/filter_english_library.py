#!/usr/bin/env python3
"""Stage English-supported entries using language metadata, never region guesses."""
import argparse
from collections import Counter, defaultdict
import csv
import hashlib
import json
from pathlib import Path
import re
import shutil

from stage_library import audit, digest, safe_child

SYSTEMS = ('Nintendo - Game Boy', 'Nintendo - Game Boy Color')
LANGUAGES = set('En Ja Fr De Es It Nl Pt Sv No Da Fi Zh Ko Pl Ca Kw Hu Ru Ar Tr Cs Sk El He Id Ro Uk Vi Th Hr Sr Bg Et Lv Lt Is La'.split())


def language_group(group):
    tokens = group.split(',')
    return tokens if tokens and all(
        re.fullmatch(r'[A-Z][a-z](?:-[A-Z]{2})?', token)
        and token.split('-')[0] in LANGUAGES for token in tokens) else []


def identity_without_languages(name):
    # Retain region, version, prototype date and every non-language qualifier.
    return re.sub(r' \(([^)]*)\)', lambda m: '' if language_group(m[1]) else m[0], name)


def english(languages):
    return any(code.split('-')[0] == 'En' for code in languages)


def make_index(metadata):
    index = defaultdict(list)
    for name, value in metadata.items():
        if value.get('languages'):
            index[identity_without_languages(name)].append((name, value['languages']))
    return index


def classify(filename, metadata, index):
    name = Path(filename).stem
    known = metadata.get(name, {}).get('languages', [])
    tagged = [code for group in re.findall(r'\(([^)]*)\)', name) for code in language_group(group)]
    if known and tagged and english(known) != english(tagged):
        return 'review', 'conflicting_metadata_and_filename', known, [name]
    if known:
        return ('include' if english(known) else 'exclude'), 'exact_metadata', known, [name]
    if tagged:
        return ('include' if english(tagged) else 'exclude'), 'explicit_filename_languages', tagged, []
    candidates = index.get(identity_without_languages(name), [])
    states = {english(languages) for _, languages in candidates}
    if len(states) == 1:
        languages = sorted(set(code for _, codes in candidates for code in codes))
        return ('include' if True in states else 'exclude'), 'language_tag_only_alias', languages, [n for n, _ in candidates]
    return 'review', ('ambiguous_language_editions' if candidates else 'no_language_evidence'), [], [n for n, _ in candidates]


def write_csv(path, rows, fields):
    with path.open('w', newline='', encoding='utf-8') as stream:
        writer = csv.DictWriter(stream, fieldnames=fields)
        writer.writeheader(); writer.writerows(rows)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--metadata', type=Path, required=True)
    parser.add_argument('--preflight', type=Path, required=True)
    args = parser.parse_args()
    source, output = args.source.resolve(), args.output.resolve()
    if output.exists() or output.is_relative_to(source):
        parser.error('Output must be a new folder outside source')
    input_manifest = (source/'reports/library-manifest.csv').read_bytes()
    rows = list(csv.DictReader(input_manifest.decode('utf-8-sig').splitlines()))
    metadata = {s:json.loads((args.metadata/(s+' (No-Intro).json')).read_text()) for s in SYSTEMS}
    indexes = {s:make_index(m) for s,m in metadata.items()}
    sources = json.loads((args.metadata/'sources.json').read_text())
    for entry in sources['sources']:
        assert digest(args.metadata/entry['file']) == entry['sha256'], entry['file']
    source_hashes = {}
    for line in (source/'reports/SHA256SUMS').read_text().splitlines():
        checksum, relative = line.split('  ', 1); source_hashes[relative] = checksum
    for row in rows:
        decision, evidence, languages, names = classify(row['rom'], metadata[row['system']], indexes[row['system']])
        row.update(language_decision=decision, language_evidence=evidence,
                   languages=','.join(languages), metadata_names=json.dumps(names, ensure_ascii=False),
                   source_library=str(source))
    kept = [r for r in rows if r['language_decision']=='include']
    output.mkdir(parents=True)
    reports = output/'reports'; reports.mkdir()
    shutil.copytree(args.metadata, reports/'language-metadata')
    checksums, art_paths = {}, []
    for index, row in enumerate(kept, 1):
        for field in ('sd_rom','sd_artwork'):
            if not row[field]: continue
            relative = row[field]
            original, target = safe_child(source, relative), safe_child(output, relative)
            if not relative.startswith('CHROMAGIC/'):
                raise ValueError('Unexpected manifest path: '+relative)
            expected = source_hashes[relative]
            if field == 'sd_rom' and expected != row['rom_sha256']:
                raise ValueError('ROM manifest disagrees with source checksums')
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(original, target)
            actual = digest(target)
            if actual != expected or digest(original) != expected:
                raise ValueError('Copy verification failed: '+relative)
            checksums[relative] = actual
            if field == 'sd_artwork':art_paths.append(target)
        if index % 500 == 0: print(f'Copied and hash-verified {index}/{len(kept)} English entries', flush=True)
    # Verify exact file inventory and status; no ROM data is modified by this tool.
    actual = {p.relative_to(output).as_posix() for p in (output/'CHROMAGIC').rglob('*') if p.is_file()}
    assert actual == set(checksums), 'Unexpected or missing staged files'
    result = audit(args.preflight.resolve(), 'rom-full', [output/r['sd_rom'] for r in kept])
    assert all(result[str(output/r['sd_rom'])] == r['loader_preflight'] for r in kept)
    assert all(s=='ok' for s in audit(args.preflight.resolve(), 'art', art_paths).values())
    fields = list(dict.fromkeys(k for r in rows for k in r))
    write_csv(reports/'language-decisions.csv', rows, fields)
    write_csv(reports/'library-manifest.csv', kept, fields)
    write_csv(reports/'excluded-non-english.csv', [r for r in rows if r['language_decision']=='exclude'], fields)
    write_csv(reports/'language-review.csv', [r for r in rows if r['language_decision']=='review'], fields)
    write_csv(reports/'rom-header-exceptions.csv', [r for r in kept if r['header_preflight']!='ok'], fields)
    write_csv(reports/'global-checksum-warnings.csv', [r for r in kept if r['loader_preflight']=='ok_global_checksum_warning'], fields)
    write_csv(reports/'rom-repairs.csv', [r for r in kept if r['repair']], fields)
    write_csv(reports/'missing-covers.csv', [r for r in kept if r['cover_kind'] in ('missing','generated_title_card')], fields)
    (reports/'SHA256SUMS').write_text(''.join(f'{h}  {n}\n' for n,h in sorted(checksums.items())))
    assert input_manifest == (source/'reports/library-manifest.csv').read_bytes(), 'Source manifest changed'
    summary = dict(source_entries=len(rows), included=len(kept), excluded_non_english=sum(r['language_decision']=='exclude' for r in rows),
                   awaiting_language_review=sum(r['language_decision']=='review' for r in rows),
                   kept_by_system=dict(Counter(r['system'] for r in kept)),
                   included_evidence=dict(Counter(r['language_evidence'] for r in kept)),
                   cover_types=dict(Counter(r['cover_kind'] for r in kept)),
                   loader_checks=dict(Counter(r['loader_preflight'] for r in kept)),
                   folder_counts=dict(sorted(Counter(str(Path(r['sd_rom']).parent.relative_to('CHROMAGIC/BACKUPS')) for r in kept).items())),
                   copied_files=len(checksums), all_copies_hash_verified=True, all_art_accepted_by_firmware_decoder=True,
                   source_manifest_sha256=hashlib.sha256(input_manifest).hexdigest(),metadata_commit=sources['commit'],
                   language_is_metadata_based_not_gameplay_verified=True, source_modified=False,
                   recommended_firmware='1.0.1-art4', firmware_rebuild_required=False, flashed=False)
    (reports/'summary.json').write_text(json.dumps(summary,indent=2)+'\n')
    (output/'README.txt').write_text(f'''ChroMagic English-focused library

{len(kept):,} English-supported ROM entries and their matching previews.
The complete collection remains in {source} and the original roms/no-intro folder.
No original files, saves, firmware, or devices were modified. No flashing occurred.

When ready, use this CHROMAGIC folder at the SD root with firmware art4. Adding it
over an existing full library will NOT remove old games; use a fresh card or replace
the old BACKUPS/ART trees after preserving saves. No SD card was changed here.

English is selected from exact No-Intro-derived language metadata or explicit
filename language tags, including En-US and multilingual entries containing En.
For renamed entries, only language tags may differ; region, revision, prototype
date, and every other qualifier must match. Conflicting/ambiguous matches are
withheld. Country/region alone is never used as proof. This is metadata matching,
not a gameplay language test. Metadata snapshot, license, URL and hash are retained
in reports/language-metadata; the catalog may not cover newer releases.
Catalog: https://github.com/unexpectedpanda/retool-clonelists-metadata
Naming: https://wiki.no-intro.org/index.php?title=Naming_Convention#Language(s)

reports/library-manifest.csv lists retained entries and original cover provenance.
reports/excluded-non-english.csv lists confirmed non-English metadata decisions.
reports/language-review.csv lists uncertain entries, also omitted from this stage.
reports/language-decisions.csv accounts for every one of the 4,661 source entries.
reports/rom-header-exceptions.csv retains known compatibility limitations among
included entries. Language filtering does not repair those games or deduplicate
versions. Generated TITLE CARD previews remain labeled substitutes for box art.
All copied bytes and preview formats were checked. Save extras remain preserved
in the full collection; none were guessed or activated in this new staging folder.
''')
    print(json.dumps(summary,indent=2),flush=True)


if __name__ == '__main__':
    main()
