#!/usr/bin/env python3
"""Packs a PlayStation Doom disc into the compressed archive the 3DS build embeds.

The game extracts this to the SD card the first time it runs. Nothing from your disc ends up in this repository: the
archive is built here, on your machine, from the copy you point at.

Only the data track and the cue sheet are packed. The audio tracks are large and the game streams music from the
extracted copy of the data track's neighbours, so they are packed too when present.
"""
import argparse
import os
import re
import sys
import zipfile


def parse_cue_files(cue_path):
    """Returns the files a cue sheet refers to, in order, as paths relative to the cue."""
    cue_dir = os.path.dirname(os.path.abspath(cue_path))
    names = []

    with open(cue_path, "r", encoding="utf-8", errors="replace") as cue:
        for line in cue:
            match = re.match(r'\s*FILE\s+"(.+?)"', line, re.IGNORECASE)

            if match:
                names.append(match.group(1))

    return [(name, os.path.join(cue_dir, name)) for name in names]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cue", required=True, help="Path to the disc's .cue sheet")
    parser.add_argument("--output", required=True, help="Where to write the archive")
    parser.add_argument(
        "--cue-name",
        required=True,
        help="What to call the cue inside the archive. The game looks for one exact name per edition, so a rip called "
             "anything else still works: only the name inside the archive has to match.",
    )
    args = parser.parse_args()

    if not os.path.isfile(args.cue):
        sys.exit("error: no such cue file: %s" % args.cue)

    entries = parse_cue_files(args.cue)

    if not entries:
        sys.exit("error: %s does not name any files" % args.cue)

    missing = [path for _, path in entries if not os.path.isfile(path)]

    if missing:
        sys.exit(
            "error: the cue sheet refers to files that are not there:\n  " + "\n  ".join(missing)
        )

    os.makedirs(os.path.dirname(os.path.abspath(args.output)), exist_ok=True)
    total = sum(os.path.getsize(path) for _, path in entries)
    print("  packing %.1f MiB from %d file(s), cue as '%s'" % (
        total / (1024.0 * 1024.0), len(entries) + 1, args.cue_name
    ))

    # Deflate rather than store: the data track compresses well and the 3DS has far more time than SD card space
    with zipfile.ZipFile(args.output, "w", zipfile.ZIP_DEFLATED, compresslevel=6) as archive:
        # The cue goes in under the name the game expects rather than whatever the rip happened to be called. Its FILE
        # lines are left alone, so they still name the track files as they are packed alongside it.
        archive.write(args.cue, args.cue_name)

        for name, path in entries:
            print("    %s" % name)
            archive.write(path, name)

    print("  wrote %s (%.1f MiB)" % (args.output, os.path.getsize(args.output) / (1024.0 * 1024.0)))


if __name__ == "__main__":
    main()
