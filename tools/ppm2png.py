# SPDX-FileCopyrightText: 2026 Jack Gu
#
# SPDX-License-Identifier: Apache-2.0

"""Convert the facepreview PPMs to PNGs, scaled up so the pixels are visible.

Stdlib only (zlib + struct), so it runs anywhere the firmware builds.
"""
import struct
import sys
import zlib


def read_ppm(path):
    with open(path, 'rb') as f:
        data = f.read()
    fields = []
    i = 0
    while len(fields) < 4:
        while data[i:i + 1].isspace():
            i += 1
        j = i
        while not data[j:j + 1].isspace():
            j += 1
        fields.append(data[i:j])
        i = j
    i += 1
    w, h = int(fields[1]), int(fields[2])
    return w, h, data[i:i + w * h * 3]


def write_png(path, w, h, rgb, scale=3):
    raw = bytearray()
    for y in range(h):
        row = rgb[y * w * 3:(y + 1) * w * 3]
        big = bytearray()
        for x in range(w):
            big += row[x * 3:x * 3 + 3] * scale
        for _ in range(scale):
            raw += b'\x00' + big

    def chunk(tag, payload):
        return (struct.pack('>I', len(payload)) + tag + payload +
                struct.pack('>I', zlib.crc32(tag + payload) & 0xffffffff))

    png = b'\x89PNG\r\n\x1a\n'
    png += chunk(b'IHDR', struct.pack('>IIBBBBB', w * scale, h * scale, 8, 2, 0, 0, 0))
    png += chunk(b'IDAT', zlib.compress(bytes(raw), 9))
    png += chunk(b'IEND', b'')
    open(path, 'wb').write(png)


for src in sys.argv[1:]:
    w, h, rgb = read_ppm(src)
    dst = src.rsplit('.', 1)[0] + '.png'
    write_png(dst, w, h, rgb)
    print(dst)
