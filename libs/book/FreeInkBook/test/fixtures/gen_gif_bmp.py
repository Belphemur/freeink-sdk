#!/usr/bin/env python3
"""Generates GIF and BMP probe fixtures (pure stdlib).

pattern.gif : valid GIF87a, 64x48, 2-color global color table, single
              first frame whose LZW stream is the "clear per pixel"
              uncompressed form (valid for standard decoders).
pattern.bmp : BITMAPINFOHEADER (40-byte DIB), 24bpp, bottom-up.
pattern_tdb.bmp : same, top-down (negative height field).
pattern_tdb_min.bmp : top-down with height field INT32_MIN (0x80000000) —
              out of the +/-0xFFFF bound, must probe as Unknown (the probe
              may not negate INT32_MIN: signed-overflow UB).
pattern_core.bmp : OS/2 BITMAPCOREHEADER (12-byte DIB), 24bpp, 24x18.

Same pattern as gen_test_image.py: left half white, right half black top /
white bottom — probe tests only parse headers, render tests (app-side) can
rely on the tone layout.
"""
import struct
import sys

W, H = 64, 48

def pixel(x, y):
    # Left half white, right half black-top/white-bottom (matches the PNG).
    if x < W // 2:
        return 1
    return 0 if y < H // 2 else 1

def write(path, blob):
    with open(path, "wb") as f:
        f.write(blob)
    print(f"gen_gif_bmp: {path} {len(blob)} bytes")

GIF_MAGIC = b"GIF87a"

def gen_gif(width=W, height=H):
    out = bytearray()
    out += GIF_MAGIC
    out += struct.pack("<HH", width, height)
    out += b"\x80"  # packed: global color table, 2 colors
    out += b"\x00\x00"  # background index, aspect
    out += bytes([255, 255, 255, 0, 0, 0])  # GCT: white, black
    # Image descriptor (no local table, non-interlaced)
    out += b"\x2c" + struct.pack("<HHHHB", 0, 0, width, height, 0)
    # LZW: min code size 2 -> clear=4, eoi=5, literal codes 3 bits wide.
    # Stream: clear, one clear before each literal makes code width fixed.
    min_code = 2
    clear = 1 << min_code
    eoi = clear + 1
    bits = 0
    acc = 0
    stream = bytearray()

    def put(code, nbits=3):
        nonlocal bits, acc
        acc |= code << bits
        bits += nbits
        while bits >= 8:
            stream.append(acc & 0xFF)
            acc >>= 8
            bits -= 8

    put(clear)
    for y in range(height):
        for x in range(width):
            put(clear)
            put(pixel(x, y))
    put(eoi)
    if bits > 0:
        stream.append(acc & 0xFF)
    out.append(min_code)
    # Sub-blocks of <=255 bytes, 0-terminated.
    for i in range(0, len(stream), 255):
        chunk = stream[i:i + 255]
        out.append(len(chunk))
        out += chunk
    out.append(0)  # block terminator
    out.append(0x3B)  # trailer
    return bytes(out)

def bmp_dib40(height_field):
    # 24bpp bottom-up unless height_field negative (top-down rows).
    stride = (W * 3 + 3) & ~3
    rows = []
    if height_field >= 0:
        order = range(H - 1, -1, -1)  # bottom-up: bottom row first
    else:
        order = range(H)              # top-down: top row first
    for y in order:
        row = bytearray()
        for x in range(W):
            v = 255 if pixel(x, y) else 0
            row += bytes([v, v, v])   # BGR
        row += b"\x00" * (stride - len(row))
        rows.append(bytes(row))
    pixels = b"".join(rows)
    off = 14 + 40
    size = off + len(pixels)
    dib = struct.pack("<IiiHHIIiiII", 40, W, height_field, 1, 24, 0,
                      len(pixels), 0, 0, 0, 0)
    return b"BM" + struct.pack("<IHHI", size, 0, 0, off) + dib + pixels

def bmp_core(w, h):
    stride = (w * 3 + 3) & ~3
    pixels = bytearray()
    for y in range(h - 1, -1, -1):
        row = bytearray()
        for x in range(w):
            v = 255 if pixel(x * W // w, y * H // h) else 0
            row += bytes([v, v, v])
        row += b"\x00" * (stride - len(row))
        pixels += row
    off = 14 + 12
    size = off + len(pixels)
    dib = struct.pack("<IHHHH", 12, w, h, 1, 24)
    return b"BM" + struct.pack("<IHHI", size, 0, 0, off) + dib + bytes(pixels)

if __name__ == "__main__":
    write(sys.argv[1] + "/pattern.gif", gen_gif())
    write(sys.argv[1] + "/pattern.bmp", bmp_dib40(H))
    write(sys.argv[1] + "/pattern_tdb.bmp", bmp_dib40(-H))
    write(sys.argv[1] + "/pattern_tdb_min.bmp", bmp_dib40(-(2**31)))
    write(sys.argv[1] + "/pattern_core.bmp", bmp_core(24, 18))
