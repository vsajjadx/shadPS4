"""Converts a stages_* dump (key 8 while frozen) to PNG files: python stages_to_png.py <dir>"""
import os
import struct
import sys
import zlib

import numpy as np


def half(a):
    return a.view(np.float16).astype(np.float32)


def unpack_r11g11b10(v):
    def uf(bits, mbits):
        e = (bits >> mbits) & 0x1F
        m = bits & ((1 << mbits) - 1)
        out = np.where(e == 0, m / (1 << mbits) * 2.0 ** -14,
                       (1 + m / (1 << mbits)) * 2.0 ** (e.astype(np.int32) - 15))
        return np.where(e == 31, 65504.0, out)
    r = uf(v & 0x7FF, 6)
    g = uf((v >> 11) & 0x7FF, 6)
    b = uf((v >> 22) & 0x3FF, 5)
    return np.stack([r, g, b], -1)


def decode(raw, w, h, fmt):
    if fmt in (37, 43):
        return np.frombuffer(raw, np.uint8).reshape(h, w, 4)[..., :3] / 255.0, False
    if fmt in (44, 50):
        return np.frombuffer(raw, np.uint8).reshape(h, w, 4)[..., 2::-1] / 255.0, False
    if fmt == 9:
        a = np.frombuffer(raw, np.uint8).reshape(h, w) / 255.0
        return np.stack([a] * 3, -1), False
    if fmt == 64:
        v = np.frombuffer(raw, np.uint32).reshape(h, w)
        return np.stack([(v & 1023), (v >> 10) & 1023, (v >> 20) & 1023], -1) / 1023.0, False
    if fmt == 122:
        return unpack_r11g11b10(np.frombuffer(raw, np.uint32).reshape(h, w)), True
    if fmt == 76:
        a = half(np.frombuffer(raw, np.uint16)).reshape(h, w)
        return np.stack([a] * 3, -1), True
    if fmt == 83:
        a = half(np.frombuffer(raw, np.uint16)).reshape(h, w, 2)
        return np.concatenate([a, np.zeros((h, w, 1), np.float32)], -1), True
    if fmt == 97:
        return half(np.frombuffer(raw, np.uint16)).reshape(h, w, 4)[..., :3], True
    if fmt == 100:
        a = np.frombuffer(raw, np.float32).reshape(h, w)
        return np.stack([a] * 3, -1), True
    if fmt == 103:
        a = np.frombuffer(raw, np.float32).reshape(h, w, 2)
        return np.concatenate([a, np.zeros((h, w, 1), np.float32)], -1), True
    if fmt == 109:
        return np.frombuffer(raw, np.float32).reshape(h, w, 4)[..., :3], True
    return None, False


def write_png(path, rgb8):
    h, w, _ = rgb8.shape
    rows = b''.join(b'\x00' + rgb8[y].tobytes() for y in range(h))

    def chunk(t, d):
        c = struct.pack('>I', len(d)) + t + d
        return c + struct.pack('>I', zlib.crc32(t + d) & 0xFFFFFFFF)
    data = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
    data += chunk(b'IDAT', zlib.compress(rows, 6)) + chunk(b'IEND', b'')
    open(path, 'wb').write(data)


d = sys.argv[1]
lines = open(os.path.join(d, 'index.txt')).read().splitlines()[1:]
for line in lines:
    name, frame, order, kind, shader, w, h, fmt, addr = line.split()
    w, h, fmt = int(w), int(h), int(fmt)
    raw = open(os.path.join(d, name), 'rb').read()
    img, hdr = decode(raw, w, h, fmt)
    if img is None:
        print('skip', name, fmt)
        continue
    img = np.nan_to_num(img.astype(np.float32), nan=1.0, posinf=1.0, neginf=0.0)
    if hdr:
        img = img / (1.0 + img)  # tone map so bright values stay visible
    rgb8 = (np.clip(img, 0, 1) * 255 + 0.5).astype(np.uint8)
    out = os.path.join(d, 'f%s_o%03d_%s_%s_%s.png' % (frame, int(order), kind, shader, name[:-4]))
    write_png(out, np.ascontiguousarray(rgb8))
print('done', len(lines))
