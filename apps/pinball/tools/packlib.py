"""
PINBALL - writing a pack (pb_pak.h has the layout) and the encodings of
its entries, shared by pack_p4.py's two ways of making the art.
"""
import os
import struct
import subprocess

import numpy as np
from PIL import Image

TOOLS = os.path.dirname(os.path.abspath(__file__))
APP = os.path.dirname(TOOLS)
BUILD = os.path.join(APP, 'build')
LZ4 = os.path.join(BUILD, 'lz4blk')

PK_TEXT, PK_RGB565, PK_SPRITE, PK_DEPTH, PK_ADD, PK_PATCH, PK_PCM, PK_ALPHA = range(8)

BAYER = np.array([[0, 8, 2, 10], [12, 4, 14, 6], [3, 11, 1, 9], [15, 7, 13, 5]], np.int32)


def lz4(data):
    src = os.path.join(TOOLS, 'lz4blk.c')
    if not os.path.exists(LZ4) or os.path.getmtime(LZ4) < os.path.getmtime(src):
        os.makedirs(BUILD, exist_ok=True)
        subprocess.check_call(['cc', '-O2', '-o', LZ4, src])
    return subprocess.run([LZ4], input=data, stdout=subprocess.PIPE, check=True).stdout


def lz4_check(src, rawlen):
    """the decoder pb_pak.c has, to check the compressor on a sample"""
    out = bytearray()
    i = 0
    while i < len(src):
        tok = src[i]
        i += 1
        lit = tok >> 4
        if lit == 15:
            while True:
                b = src[i]
                i += 1
                lit += b
                if b != 255:
                    break
        out.extend(src[i:i + lit])
        i += lit
        if i >= len(src):
            break
        off = src[i] | (src[i + 1] << 8)
        i += 2
        m = (tok & 15) + 4
        if (tok & 15) == 15:
            while True:
                b = src[i]
                i += 1
                m += b
                if b != 255:
                    break
        start = len(out) - off
        for k in range(m):
            out.append(out[start + k])
    assert len(out) == rawlen, (len(out), rawlen)
    return bytes(out)


def rgb565(a, dither=True):
    """an (h, w, 3) uint8 array to little-endian RGB565, ordered dither"""
    a = np.asarray(a)[..., :3].astype(np.int32)
    h, w = a.shape[:2]
    if dither:
        d = np.tile(BAYER, (h // 4 + 1, w // 4 + 1))[:h, :w]
        r = np.clip(a[..., 0] + (d >> 1) - 4, 0, 255)
        g = np.clip(a[..., 1] + (d >> 2) - 2, 0, 255)
        b = np.clip(a[..., 2] + (d >> 1) - 4, 0, 255)
    else:
        r, g, b = a[..., 0], a[..., 1], a[..., 2]
    return (((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)).astype('<u2')


def crop_box(mask, pad=0):
    ys, xs = np.nonzero(mask)
    if len(xs) == 0:
        return None
    h, w = mask.shape
    return (max(0, xs.min() - pad), min(w, xs.max() + 1 + pad), max(0, ys.min() - pad), min(h, ys.max() + 1 + pad))


def mulaw(x):
    """float samples -1..1 to G.711 mu-law bytes"""
    x = np.clip(np.asarray(x, np.float64), -1, 1)
    s = (x * 32635).astype(np.int32)
    sign = (s < 0)
    s = np.abs(s) + 0x84
    s = np.minimum(s, 32635 + 0x84)
    exp = np.floor(np.log2(np.maximum(s, 1))).astype(np.int32) - 7
    exp = np.clip(exp, 0, 7)
    man = (s >> (exp + 3)) & 0x0F
    u = ~((sign.astype(np.int32) << 7) | (exp << 4) | man) & 0xFF
    return u.astype(np.uint8).tobytes()


class Pack:
    def __init__(self):
        self.entries = []

    def add(self, name, typ, w, h, x, y, raw, extra=0, flags=0, compress=True):
        assert len(name.encode()) < 24, name
        assert all(e['name'] != name for e in self.entries), name
        c = lz4(raw) if compress and len(raw) > 64 else raw
        if len(c) >= len(raw):
            c = raw
        elif len(self.entries) % 12 == 0 and len(raw) < 3_000_000:
            assert lz4_check(c, len(raw)) == raw, name
        self.entries.append(dict(name=name, type=typ, w=int(w), h=int(h), x=int(x), y=int(y), raw=len(raw),
                                 data=c, extra=int(extra), flags=flags))

    def text(self, name, s):
        self.add(name, PK_TEXT, 0, 0, 0, 0, s.encode('utf-8'))

    def rgb(self, name, img, x=0, y=0):
        a = np.asarray(img.convert('RGB'))
        self.add(name, PK_RGB565, a.shape[1], a.shape[0], x, y, rgb565(a).tobytes())

    def sprite(self, name, img, x=0, y=0, crop=True):
        """an RGBA picture placed at (x, y) on the screen; cropped to what
        is not transparent, the offset kept"""
        a = np.asarray(img.convert('RGBA'))
        if crop:
            box = crop_box(a[..., 3] > 0)
            if box is None:
                return False
            x0, x1, y0, y1 = box
            a = a[y0:y1, x0:x1]
            x += x0
            y += y0
        # colours under a faint alpha bleed dark when blended: un-premultiply
        raw = rgb565(a[..., :3]).tobytes() + a[..., 3].astype(np.uint8).tobytes()
        self.add(name, PK_SPRITE, a.shape[1], a.shape[0], x, y, raw)
        return True

    def spritez(self, name, img, z, x, y):
        """a toy's frame: colour, alpha and the depth of each pixel (mm)"""
        a = np.asarray(img.convert('RGBA'))
        box = crop_box(a[..., 3] > 0)
        if box is None:
            return False
        x0, x1, y0, y1 = box
        c = a[y0:y1, x0:x1]
        zz = np.clip(np.round(np.asarray(z)[y0:y1, x0:x1] * 10), 0, 65000).astype('<u2')
        raw = rgb565(c[..., :3]).tobytes() + c[..., 3].astype(np.uint8).tobytes() + zz.tobytes()
        self.add(name, 8, x1 - x0, y1 - y0, x + x0, y + y0, raw)
        return True

    def depth(self, name, z, x=0, y=0):
        """z in millimetres -> u16 tenths"""
        z = np.clip(np.round(np.asarray(z) * 10), 0, 65000).astype('<u2')
        self.add(name, PK_DEPTH, z.shape[1], z.shape[0], x, y, z.tobytes())

    def add_light(self, name, light, x, y, cut=3):
        """light added by a lamp: an (h, w, 3) array 0..255, cropped"""
        light = np.asarray(light).astype(np.int32)
        m = light.max(axis=2) > cut
        box = crop_box(m)
        if box is None:
            return False
        x0, x1, y0, y1 = box
        l = light[y0:y1, x0:x1]
        v = rgb565(np.clip(l, 0, 255).astype(np.uint8), dither=False)
        v[~m[y0:y1, x0:x1]] = 0
        self.add(name, PK_ADD, l.shape[1], l.shape[0], x + x0, y + y0, v.tobytes())
        return True

    def patch(self, name, rgb, z, mask, x, y):
        box = crop_box(mask)
        if box is None:
            return False
        x0, x1, y0, y1 = box
        c = rgb565(np.asarray(rgb)[y0:y1, x0:x1]).tobytes()
        zz = np.clip(np.round(np.asarray(z)[y0:y1, x0:x1] * 10), 0, 65000).astype('<u2').tobytes()
        m = (np.asarray(mask)[y0:y1, x0:x1] > 0).astype(np.uint8).tobytes()
        self.add(name, PK_PATCH, x1 - x0, y1 - y0, x + x0, y + y0, c + zz + m)
        return True

    def pcm(self, name, samples, rate=16000):
        self.add(name, PK_PCM, 0, 0, 0, 0, mulaw(samples), extra=rate)

    def write(self, path):
        os.makedirs(os.path.dirname(path), exist_ok=True)
        n = len(self.entries)
        off = 8 + n * 52
        tab = b''
        for e in self.entries:
            nm = e['name'].encode().ljust(24, b'\0')
            tab += nm + struct.pack('<BBHHhhIIIIH', e['type'], e['flags'], e['w'], e['h'], e['x'], e['y'], off,
                                    len(e['data']), e['raw'], e['extra'], 0)
            off += len(e['data'])
        with open(path, 'wb') as f:
            f.write(b'PBPK' + struct.pack('<HH', 1, n) + tab)
            for e in self.entries:
                f.write(e['data'])
        return off
