#!/usr/bin/env python3
"""Checks a banner .wav against what the 3DS will actually accept.

Nothing warns when this is wrong. The banner simply plays ringing noise instead of music, so it is worth checking
rather than discovering on hardware. See packaging/BANNER_ASSETS.md for how these numbers were arrived at.
"""
import struct
import sys

WANT_RATE = 16364
WANT_CHANNELS = 2
WANT_BITS = 16
WANT_FRAMES = 49092      # Exactly three seconds at 16364 Hz


def main():
    if len(sys.argv) != 2:
        sys.exit("usage: check_banner_audio.py <banner.wav>")

    path = sys.argv[1]

    with open(path, "rb") as wav:
        data = wav.read()

    if data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        sys.exit("error: %s is not a WAV file" % path)

    fmt_at = data.index(b"fmt ") + 8
    _, channels, rate, _, block_align, bits = struct.unpack("<HHIIHH", data[fmt_at:fmt_at + 16])

    data_at = data.index(b"data") + 4
    data_size = struct.unpack("<I", data[data_at:data_at + 4])[0]
    frames = data_size // block_align if block_align else 0

    problems = []

    if rate != WANT_RATE:
        problems.append("sample rate is %d, must be %d" % (rate, WANT_RATE))
    if channels != WANT_CHANNELS:
        problems.append("%d channel(s), must be %d" % (channels, WANT_CHANNELS))
    if bits != WANT_BITS:
        problems.append("%d bits per sample, must be %d" % (bits, WANT_BITS))
    if frames > WANT_FRAMES:
        problems.append(
            "%d frames (%.2fs), must be at most %d (3.00s) or it wraps and plays as noise"
            % (frames, frames / float(rate or 1), WANT_FRAMES)
        )

    if problems:
        sys.exit("error: banner audio is not usable:\n  " + "\n  ".join(problems))


if __name__ == "__main__":
    main()
