#!/usr/bin/env python3
"""Convert an xwd dump to PNG.

Written because the obvious decode is wrong in a way that looks like a
renderer bug. An X server on a 24-bit visual reports bits_per_pixel 24 and
pads each row to a 32-bit boundary, so decoding 4 bytes per pixel drifts by
one byte per pixel and produces a 3-pixel rotating colour stripe. The first
native screenshots of Folio looked like a broken shader for exactly that
reason; the renderer was fine.
"""
import struct
import sys

from PIL import Image


def load(path):
    data = open(path, "rb").read()
    f = struct.unpack(">25I", data[:100])
    header, depth, w, h = f[0], f[3], f[4], f[5]
    bits_per_pixel, bytes_per_line, ncolors = f[11], f[12], f[19]
    # The colormap sits between the header and the pixels.
    pixels = data[header + ncolors * 12:]
    mode = "BGRX" if bits_per_pixel == 32 else "BGR"
    return Image.frombytes("RGB", (w, h), pixels[:h * bytes_per_line],
                           "raw", mode, bytes_per_line)


if __name__ == "__main__":
    img = load(sys.argv[1])
    if len(sys.argv) > 4:
        img = img.crop((0, 0, int(sys.argv[3]), int(sys.argv[4])))
    img.save(sys.argv[2])
