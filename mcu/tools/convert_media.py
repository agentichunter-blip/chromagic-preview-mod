#!/usr/bin/env python3
"""Convert one user-supplied cover or gameplay clip without a library manifest."""
import argparse
from pathlib import Path
from prepare_previews import convert_cover, convert_video


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("kind", choices=("cover", "video"))
    parser.add_argument("source", type=Path)
    parser.add_argument("destination", type=Path)
    args = parser.parse_args()
    if not args.source.is_file():
        parser.error("Source must be an existing file")
    if args.source.resolve() == args.destination.resolve():
        parser.error("Source and destination must differ")
    if args.destination.exists():
        parser.error("Destination already exists; choose a new output path")
    expected = ".cma" if args.kind == "cover" else ".cmv"
    if args.destination.suffix != expected:
        parser.error(f"Destination must end in {expected}")
    try:
        converter = convert_cover if args.kind == "cover" else convert_video
        converter(args.source, args.destination)
    except (OSError, ValueError) as exc:
        parser.exit(1, f"Conversion failed: {exc}\n")
    print(f"Wrote {args.destination}")


if __name__ == "__main__":
    main()
