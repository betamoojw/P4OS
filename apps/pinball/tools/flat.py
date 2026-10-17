"""
PINBALL - the art without Blender: the table drawn with PIL through the
game's own camera, flat-shaded. It is how the geometry is tried in the
simulator before the renders exist (pack_p4.py --flat), and a table that
works here works the same with the renders: only the pictures change.

It makes the same pieces build_table.py renders (see art_dict()).
"""
import math

import numpy as np
from PIL import Image, ImageDraw, ImageFilter

import paint
import pbtable as pt

TW, TH = pt.SCREEN_W, pt.SCREEN_H - pt.DMD_H
FLIP_FRAMES = 12
SPIN_FRAMES = 8


def rgb(c, k=1.0):
    return paint.rgb(c, k)


def homography(src, dst):
    """the 3x3 H with dst ~ H src for four point pairs"""
    A = []
    b = []
    for (x, y), (u, v) in zip(src, dst):
        A.append([x, y, 1, 0, 0, 0, -u * x, -u * y])
        A.append([0, 0, 0, x, y, 1, -v * x, -v * y])
        b += [u, v]
    h = np.linalg.solve(np.array(A, float), np.array(b, float))
    return np.append(h, 1).reshape(3, 3)


class Flat:
    def __init__(self, t):
        self.t = t
        self.c = t.camera

    def S(self, x, y, z=0.0):
        sx, sy, _ = self.c.project(x, y, z)
        return (sx, sy - pt.DMD_H)

    def playfield(self):
        tex = paint.paint_playfield(self.t)
        K = paint.K
        # the plane z = 0: screen -> texture pixels
        corners = [(0, 0), (pt.W, 0), (pt.W, pt.H), (0, pt.H)]
        scr = [self.S(x, y) for x, y in corners]
        tx = [(x * K, y * K) for x, y in corners]
        Hm = homography(scr, tx)
        coeffs = (Hm / Hm[2, 2]).flatten()[:8]
        img = tex.transform((TW, TH), Image.PERSPECTIVE, tuple(coeffs), Image.BILINEAR, fillcolor=None)
        # outside the playfield: the cabinet
        mask = Image.new('L', (TW, TH), 0)
        ImageDraw.Draw(mask).polygon(scr, fill=255)
        bg = Image.new('RGB', (TW, TH), rgb(self.t.theme.get('cabinet', 0x201810)))
        bg.paste(img, (0, 0), mask)
        return bg

    # ---- solids ---------------------------------------------------------
    def wall_faces(self, d, pts, h, thick, col_side, col_top):
        for a, b in zip(pts, pts[1:]):
            q = [self.S(a[0], a[1], 0), self.S(b[0], b[1], 0), self.S(b[0], b[1], h), self.S(a[0], a[1], h)]
            d.polygon(q, fill=col_side)
        left = pt.offset_line(pts, thick / 2)
        right = pt.offset_line(pts, -thick / 2)
        for i in range(len(pts) - 1):
            q = [self.S(*left[i], h), self.S(*left[i + 1], h), self.S(*right[i + 1], h), self.S(*right[i], h)]
            d.polygon(q, fill=col_top)

    def cylinder(self, d, x, y, r, z0, z1, side, top, outline=None):
        n = 20
        ring = [(x + r * math.cos(i / n * 2 * math.pi), y + r * math.sin(i / n * 2 * math.pi)) for i in range(n)]
        lo = [self.S(px, py, z0) for px, py in ring]
        hi = [self.S(px, py, z1) for px, py in ring]
        for i in range(n):
            j = (i + 1) % n
            d.polygon([lo[i], lo[j], hi[j], hi[i]], fill=side)
        d.polygon(hi, fill=top, outline=outline)

    def items(self):
        """what stands on the field, far ones first"""
        def key(v):
            if 'y' in v:
                return v['y']
            if 'pts' in v:
                return max(p[1] for p in v['pts'])
            if 'path' in v:
                return min(p[1] for p in v['path'])
            if 'a' in v:
                return max(v['a'][1], v['b'][1])
            return 0
        return sorted(self.t.vis, key=key)

    def solids(self, img, skip=()):
        th = self.t.theme
        d = ImageDraw.Draw(img)
        acc = rgb(th['accent'])
        for v in self.items():
            k = v['t']
            if k in skip:
                continue
            if k == 'wall':
                style = v['style']
                if style == 'side':
                    side, top = rgb(th.get('wood', 0x402818), 0.8), rgb(th.get('wood', 0x402818), 1.3)
                elif style == 'plastic':
                    side, top = rgb(th.get('plastic', th['accent']), 0.5), rgb(th.get('plastic', th['accent']))
                elif style == 'apron':
                    continue
                else:
                    side, top = rgb(th.get('rail', 0x9098A0), 0.55), rgb(th.get('rail', 0x9098A0))
                self.wall_faces(d, v['pts'], v['h'], v['thick'], side, top)
            elif k == 'rubber':
                self.wall_faces(d, v['pts'], v['h'], 5, (20, 20, 20), (40, 40, 40))
            elif k == 'post':
                self.cylinder(d, v['x'], v['y'], v['r'], 0, 26, (30, 30, 30) if v['rubber'] else (140, 140, 150),
                              (60, 60, 60) if v['rubber'] else (200, 200, 210))
            elif k == 'bumper':
                body = rgb(v['body'] or 0x404040)
                cap = rgb(v['cap'] or th['accent'], 0.55)
                self.cylinder(d, v['x'], v['y'], v['r'] + 3, 0, 12, (25, 25, 25), (60, 60, 60))
                self.cylinder(d, v['x'], v['y'], v['r'] - 2, 12, 36, body, rgb(v['body'] or 0x404040, 1.3))
                self.cylinder(d, v['x'], v['y'], v['r'] + 1, 36, 46, cap, cap, outline=(230, 230, 230))
            elif k == 'sling':
                a, b, c = v['pts']
                col = rgb(v['color'] or th['accent'], 0.5)
                for p, q in ((a, b), (b, c), (c, a)):
                    d.polygon([self.S(*p, 0), self.S(*q, 0), self.S(*q, 30), self.S(*p, 30)], fill=(30, 30, 30))
                d.polygon([self.S(*a, 30), self.S(*b, 30), self.S(*c, 30)], fill=col)
            elif k in ('drop', 'target'):
                a, b = v['a'], v['b']
                col = rgb(v['color'] or th['accent'])
                d.polygon([self.S(*a, 0), self.S(*b, 0), self.S(*b, 32), self.S(*a, 32)], fill=col,
                          outline=(0, 0, 0))
            elif k == 'spinner':
                a, b = v['a'], v['b']
                for p in (a, b):
                    self.cylinder(d, p[0], p[1], 2.5, 0, 40, (150, 150, 160), (200, 200, 210))
                d.line([self.S(*a, 40), self.S(*b, 40)], fill=(200, 200, 210), width=2)
            elif k == 'saucer':
                x, y, r = v['x'], v['y'], v['r']
                ring = [self.S(x + r * math.cos(i / 20 * 2 * math.pi), y + r * math.sin(i / 20 * 2 * math.pi))
                        for i in range(20)]
                d.polygon(ring, fill=(8, 8, 8), outline=(180, 180, 190))
            elif k == 'ramp':
                self.ramp(d, v)
            elif k == 'gate':
                d.line([self.S(*v['a'], 20), self.S(*v['b'], 20)], fill=(210, 210, 220), width=2)
            elif k == 'insert':
                l = dict(v)
                pts = [self.S(*p) for p in paint.lamp_shape(l)]
                d.polygon(pts, fill=rgb(v['color'], 0.28))
            elif k == 'decor':
                self.cylinder(d, v['x'], v['y'], 12 * v.get('s', 1.0), 0, 40 * v.get('s', 1.0), (60, 50, 70),
                              rgb(th['accent'], 0.7))
            elif k == 'apron':
                y = v['y']
                q = [self.S(0, y), self.S(pt.LANE_X, y), self.S(pt.LANE_X, pt.H + 4, 30), self.S(0, pt.H + 4, 30)]
                # the apron leaves the flippers' gap open
                d.polygon(q, fill=rgb(th.get('apron', th['panel']), 1.4))
                gap = [self.S(110, y), self.S(372, y), self.S(372, pt.H + 4, 30), self.S(110, pt.H + 4, 30)]
                d.polygon(gap, fill=(6, 6, 8))
            elif k == 'plunger':
                self.cylinder(d, v['x'], v['y'] + 14, 6, 0, 14, (150, 150, 160), (210, 210, 220))
        return img

    def ramp(self, d, v):
        path = v['path']
        w = v['width'] / 2
        pts2 = [(p[0], p[1]) for p in path]
        L = pt.offset_line(pts2, w)
        R = pt.offset_line(pts2, -w)
        col = rgb(v['color'] or self.t.theme['accent'], 0.75)
        edge = rgb(v['color'] or self.t.theme['accent'], 1.2)
        for i in range(len(path) - 1):
            z0, z1 = path[i][2], path[i + 1][2]
            q = [self.S(*L[i], z0), self.S(*L[i + 1], z1), self.S(*R[i + 1], z1), self.S(*R[i], z0)]
            d.polygon(q, fill=col)
            for side in (L, R):
                d.line([self.S(*side[i], z0 + 14), self.S(*side[i + 1], z1 + 14)], fill=edge, width=3)

    # ---- the pieces --------------------------------------------------------
    def lamps(self, base):
        """each lamp: the light it adds, as a full table-area array"""
        out = {}
        th = self.t.theme
        for l in self.t.lamps:
            img = Image.new('RGB', (TW, TH), (0, 0, 0))
            d = ImageDraw.Draw(img)
            col = rgb(l['color'])
            if l['kind'] == 'insert':
                pts = [self.S(*p) for p in paint.lamp_shape(l)]
                d.polygon(pts, fill=col)
                glow = img.filter(ImageFilter.GaussianBlur(9))
                a = np.asarray(img).astype(np.int32) * 0.85 + np.asarray(glow).astype(np.int32) * 0.9
            elif l['kind'] == 'bumper':
                x, y, r = l['x'], l['y'], l['size']
                ring = [self.S(x + r * math.cos(i / 20 * 2 * math.pi), y + r * math.sin(i / 20 * 2 * math.pi), 46)
                        for i in range(20)]
                d.polygon(ring, fill=col)
                glow = img.filter(ImageFilter.GaussianBlur(14))
                a = np.asarray(img).astype(np.int32) * 0.7 + np.asarray(glow).astype(np.int32) * 1.2
            elif l['kind'] == 'sling':
                sl = [v for v in self.t.vis if v['t'] == 'sling' and v.get('lamp') == l['name']]
                if sl:
                    d.polygon([self.S(*p, 30) for p in sl[0]['pts']], fill=col)
                a = np.asarray(img).astype(np.int32) * 0.8
            else:
                x, y = self.S(l['x'], l['y'], l.get('z', 0))
                r = l.get('size', 60)
                d.ellipse([x - r, y - r, x + r, y + r], fill=col)
                a = np.asarray(img.filter(ImageFilter.GaussianBlur(r * 0.6))).astype(np.int32) * 0.7
            out[l['name']] = np.clip(a, 0, 255)
        return out

    def flipper_frames(self, f):
        th = self.t.theme
        frames = []
        for k in range(FLIP_FRAMES):
            ang = f['rest'] + (f['up'] - f['rest']) * k / (FLIP_FRAMES - 1)
            a = math.radians(ang if f['side'] == 'L' else 180 - ang)
            img = Image.new('RGBA', (TW, TH), (0, 0, 0, 0))
            d = ImageDraw.Draw(img)
            outline = []
            px, py, L, r0, r1 = f['x'], f['y'], f['len'], f['r0'], f['r1']
            tx, ty = px + L * math.cos(a), py + L * math.sin(a)
            for i in range(13):
                b = a + math.pi / 2 + math.pi * i / 12
                outline.append((px + r0 * math.cos(b), py + r0 * math.sin(b)))
            for i in range(13):
                b = a - math.pi / 2 + math.pi * i / 12
                outline.append((tx + r1 * math.cos(b), ty + r1 * math.sin(b)))
            lo = [self.S(x, y, 2) for x, y in outline]
            hi = [self.S(x, y, 24) for x, y in outline]
            for i in range(len(outline)):
                j = (i + 1) % len(outline)
                d.polygon([lo[i], lo[j], hi[j], hi[i]], fill=rgb(th.get('flipper_rubber', 0xC02020)) + (255,))
            d.polygon(hi, fill=rgb(th.get('flipper', 0xF0F0F0)) + (255,))
            cx, cy = self.S(px, py, 26)
            d.ellipse([cx - 4, cy - 4, cx + 4, cy + 4], fill=(150, 150, 160, 255))
            frames.append(img)
        return frames

    def spinner_frames(self):
        sp = [v for v in self.t.vis if v['t'] == 'spinner']
        if not sp:
            return []
        v = sp[0]
        a, b = v['a'], v['b']
        frames = []
        for k in range(SPIN_FRAMES):
            ang = k / SPIN_FRAMES * 2 * math.pi
            img = Image.new('RGBA', (TW, TH), (0, 0, 0, 0))
            d = ImageDraw.Draw(img)
            # the plate hangs from the wire at z = 40, 30 mm tall
            dx, dz = math.sin(ang) * 30, -math.cos(ang) * 30
            nx, ny = (b[1] - a[1]), -(b[0] - a[0])
            l = math.hypot(nx, ny) or 1
            nx, ny = nx / l * dx, ny / l * dx
            q = [self.S(a[0] + 3, a[1], 40), self.S(b[0] - 3, b[1], 40), self.S(b[0] - 3 + nx, b[1] + ny, 40 + dz),
                 self.S(a[0] + 3 + nx, a[1] + ny, 40 + dz)]
            shade = 0.6 + 0.4 * abs(math.cos(ang))
            d.polygon(q, fill=rgb(v['color'] or 0xD0D0D8, shade) + (255,))
            frames.append(img)
        return frames

    def button_icon(self, size, col, pressed=False, glyph='launch'):
        img = Image.new('RGBA', (size, size), (0, 0, 0, 0))
        d = ImageDraw.Draw(img)
        k = 0.8 if pressed else 1.0
        d.ellipse([4, 4, size - 4, size - 4], fill=rgb(col, 0.35 * k) + (235,), outline=rgb(col, 1.2) + (255,),
                  width=6)
        if glyph == 'launch':
            c = size / 2
            d.polygon([(c - size * 0.18, c + size * 0.12), (c, c - size * 0.22), (c + size * 0.18, c + size * 0.12)],
                      fill=(255, 255, 255, 255))
        else:
            c = size / 2
            w = size * 0.1
            d.rectangle([c - w * 2, c - size * 0.2, c - w * 0.7, c + size * 0.2], fill=(255, 255, 255, 255))
            d.rectangle([c + w * 0.7, c - size * 0.2, c + w * 2, c + size * 0.2], fill=(255, 255, 255, 255))
        return img

    def art(self):
        bg = self.playfield()
        base = self.solids(bg.copy())
        th = self.t.theme
        art = dict(bg=base, depth=None, lamps=self.lamps(base), drops={},
                   flippers={i: self.flipper_frames(f) for i, f in enumerate(self.t.flippers)},
                   spinner=self.spinner_frames(), ball=None,
                   launch=self.button_icon(150, th['accent']), launch_on=self.button_icon(150, th['accent'], True),
                   pause=self.button_icon(88, th['accent'], glyph='pause'), icon=None)
        art['icon'] = art['launch']
        return art
